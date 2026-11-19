#!/usr/bin/env python3
"""BLE peripheral that plays a HiFlow Pro: GATT service e0ff with ffe1 (write)
and ffe2 (notify); every written frame goes to fake_inverter.c (libfake.so),
the reply goes back as notifications. Bench use only, see README.md."""

import asyncio
import ctypes
import json
import os
from pathlib import Path
import signal
import time

from bless import BlessServer
from bless.backends.bluezdbus.dbus import application as bless_app
from bless.backends.bluezdbus.dbus import characteristic as bless_char
from bless.backends.bluezdbus.dbus import service as bless_service
from bless import GATTAttributePermissions as A
from bless import GATTCharacteristicProperties as P
from dbus_next import BusType
from dbus_next.aio import MessageBus
from dbus_next.errors import DBusError, InterfaceNotFoundError
from dbus_next.service import method

HERE = Path(__file__).resolve().parent
SERVICE = "0000e0ff-3c17-d293-8e48-14fe2e4da212"
TX = "0000ffe1-0000-1000-8000-00805f9b34fb"  # bridge writes
RX = "0000ffe2-0000-1000-8000-00805f9b34fb"  # bridge gets notified
CHUNK = int(os.environ.get("CHUNK", "20"))
KEY = bytes.fromhex(os.environ.get("KEY", "00112233445566778899aabbccddeeff"))
SN = os.environ.get("SN", "0000000000AA").encode()
PIN = os.environ.get("PIN", "").encode()
# A real inverter advertises as RMI- and its serial; a bridge without a MAC in its
# build finds it by that name.
NAME = os.environ.get("NAME", "RMI-" + SN.decode())
# The adapter to run on (hci0, hci1, ...), default the first one BlueZ lists.
ADAPTER = os.environ.get("ADAPTER", "")
# While a bridge holds its link to the fake, the controller may refuse new connections
# to other devices. So the fake refuses an adapter with paired or connected devices
# unless SHARED=1.
SHARED = os.environ.get("SHARED") == "1"


class _NoUuids(list):
    def append(self, item):
        pass


class _FullNameAdvertisement(bless_app.BlueZLEAdvertisement):
    """bless puts the 128-bit service UUID into the advertisement, which leaves room
    for only 8 of the name's 16 characters (BlueZ shortens it). A real inverter
    sends its whole name; the bridge connects by address and finds the service
    after connecting. So the UUID stays out and the name goes out whole."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._service_uuids = _NoUuids()


bless_app.BlueZLEAdvertisement = _FullNameAdvertisement

central = None  # D-Bus path of the device that wrote the last frame


class _WriterCharacteristic(bless_char.BlueZGattCharacteristic):
    """bless drops the options of a write. BlueZ names the writing device there: the
    central a kill drops, never another device on the adapter."""

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):  # type: ignore # noqa: F722 F821 N802
        global central
        if "device" in options:
            central = options["device"].value
        bless_char.BlueZGattCharacteristic.WriteValue(self, value, options)


bless_service.BlueZGattCharacteristic = _WriterCharacteristic
assert len(KEY) == 16, "KEY must be 16 bytes (32 hex digits)"

# The recorded two-page reading of the host tests.
PAGES = json.loads((HERE.parents[1] / "test" / "vectors.json").read_text())["protobuf_realdata_new_pages"]

lib = ctypes.CDLL(str(HERE / "libfake.so"))
lib.shim_new.restype = ctypes.c_void_p
lib.shim_new.argtypes = [ctypes.c_char_p] * 5
lib.shim_link_up.argtypes = [ctypes.c_void_p]
lib.shim_set_time.argtypes = [ctypes.c_void_p, ctypes.c_int64]
lib.shim_frame.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
]
lib.shim_counts.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
SHIM_COUNTS = 10  # ints that shim_counts() fills
lib.shim_set_power.argtypes = [ctypes.c_void_p, ctypes.c_int32]
lib.shim_reset_pages.argtypes = [ctypes.c_void_p]
lib.shim_set_mode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.shim_last_login_lead.restype = ctypes.c_int64
lib.shim_last_login_lead.argtypes = [ctypes.c_void_p]
lib.shim_last_ble_id.restype = ctypes.c_char_p
lib.shim_last_ble_id.argtypes = [ctypes.c_void_p]
fi = lib.shim_new(KEY, SN, PIN, PAGES["page0_hex"].encode(), PAGES["page1_hex"].encode())


def log(*a):
    print(time.strftime("%T"), *a, flush=True)


server = None
loop = None
pending = []

# "power" next to this script: the AC power in watts the fake reports (missing
# or empty = the recorded pages). Read on every frame, so a dusk is one echo
# away. Only the power changes; serial and energies stay as recorded.
POWER_FILE = HERE / "power"
MAX_POWER_W = 10000
current_power = ""


def apply_power_file():
    global current_power
    try:
        txt = POWER_FILE.read_text().strip()
    except OSError:
        txt = ""
    if txt == current_power:
        return
    if not txt:
        lib.shim_reset_pages(fi)
        log("power: the recorded pages again")
    else:
        try:
            watts = float(txt)
        except ValueError:
            watts = -1
        if not 0 <= watts <= MAX_POWER_W:
            log(f"power: {txt!r} is not a number of watts 0-{MAX_POWER_W}, ignored")
            current_power = txt  # log once per content
            return
        if lib.shim_set_power(fi, round(watts * 10)) != 0:
            log("power: the recorded pages do not decode, ignored")
            current_power = txt
            return
        log(f"power now {watts:g} W")
    current_power = txt


async def bluez_objects(bus):
    intro = await bus.introspect("org.bluez", "/")
    manager = bus.get_proxy_object("org.bluez", "/", intro).get_interface("org.freedesktop.DBus.ObjectManager")
    return await manager.call_get_managed_objects()


async def pick_adapter():
    """ADAPTER or the first adapter. Refuses one that other devices use, unless SHARED=1."""
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    try:
        objects = await bluez_objects(bus)
    finally:
        bus.disconnect()
    adapters = sorted(path.rsplit("/", 1)[1] for path, ifaces in objects.items() if "org.bluez.Adapter1" in ifaces)
    name = ADAPTER or (adapters[0] if adapters else "")
    if name not in adapters:
        raise SystemExit(f"no Bluetooth adapter {name!r} (there are: {' '.join(adapters) or 'none'})")
    devices = [ifaces["org.bluez.Device1"] for path, ifaces in objects.items()
               if path.startswith(f"/org/bluez/{name}/") and "org.bluez.Device1" in ifaces]
    others = sorted(d["Alias"].value for d in devices if d["Paired"].value or d["Connected"].value)
    if others and not SHARED:
        raise SystemExit(
            f"{name} is in use by: {', '.join(others)}. While a bridge holds its link to the fake, the "
            "controller may refuse new connections to these devices. Run the fake on an adapter of its own "
            "(ADAPTER=hciN) or accept that with SHARED=1.")
    return name


async def drop_link(path):
    """The fake drops the link: disconnect the central that wrote the frame, no other device."""
    if path is None:
        log("KILL: no central has written yet, nothing dropped")
        return
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    try:
        intro = await bus.introspect("org.bluez", path)
        device = bus.get_proxy_object("org.bluez", path, intro).get_interface("org.bluez.Device1")
        await device.call_disconnect()
        log(f"KILL: link to {path.rsplit('/', 1)[1]} dropped")
    except (DBusError, InterfaceNotFoundError) as e:
        log(f"KILL: {path} not dropped: {e}")
    finally:
        # One connection per kill: refused logins kill the link every few minutes.
        bus.disconnect()


# "mode" next to this script (bench scenarios), words on one line:
#   refuse     kill the link on every login
#   rotate     hand out a new key with every V0 pairing
#   clock N    the device clock runs N seconds off the laptop's
#   lead N     kill the link on a request (not the V0) N seconds or more ahead of the device clock
#   lag N      kill the link on a request (not the V0) N seconds or more behind the device clock
MODE_FILE = HERE / "mode"
current_mode = None
clock_offset = 0


def apply_mode_file():
    global current_mode, clock_offset
    try:
        txt = MODE_FILE.read_text().strip()
    except OSError:
        txt = ""
    if txt == current_mode:
        return
    current_mode = txt  # log once per content, also when it is bad
    words = txt.split()

    def number(word):
        return int(words[words.index(word) + 1]) if word in words else 0

    try:
        clock, lead, lag = number("clock"), number("lead"), number("lag")
    except (IndexError, ValueError):
        log(f"mode: {txt!r} has a word without a whole number after it, ignored")
        return
    refuse = "refuse" in words
    rotate = "rotate" in words
    clock_offset = clock
    lib.shim_set_mode(fi, int(refuse), int(rotate), lead, lag)
    log(f"mode: refuse={refuse} rotate={rotate} clock={clock_offset:+d} s lead={lead} lag={lag}")


def on_write(characteristic, value, **kwargs):
    apply_mode_file()
    lib.shim_set_time(fi, int(time.time()) + clock_offset)
    apply_power_file()
    counts = (ctypes.c_int * SHIM_COUNTS)()
    lib.shim_counts(fi, counts)
    logins_before = counts[1]
    limit_writes_before, power_commands_before = counts[6], counts[8]
    out = ctypes.create_string_buffer(2048)
    kill = ctypes.c_int(0)
    n = lib.shim_frame(fi, bytes(value), len(value), out, ctypes.byref(kill))
    log(f"<- {len(value)} B, -> {n} B{' KILL' if kill.value else ''}")
    lib.shim_counts(fi, counts)
    if counts[1] != logins_before:
        log(f"LOGIN #{counts[1]}: bleId {lib.shim_last_ble_id(fi).decode(errors='replace')},"
            f" timestamp {lib.shim_last_login_lead(fi):+d} s vs device clock"
            f"{' (killed)' if kill.value else ''}")
    if counts[6] != limit_writes_before:
        log(f"LIMIT write #{counts[6]}: limit now {counts[7] / 10:g} %")
    if counts[8] != power_commands_before:
        log(f"POWER command #{counts[8]}: output now {'on' if counts[9] else 'off'}")
    if n:
        pending.append(out.raw[:n])
    if kill.value:
        writer = central  # this frame's writer, even if another central writes before the drop
        loop.call_soon_threadsafe(lambda: asyncio.ensure_future(drop_link(writer)))


async def pump():
    while True:
        while pending:
            data = pending.pop(0)
            for i in range(0, len(data), CHUNK):
                server.get_characteristic(RX).value = bytearray(data[i:i + CHUNK])
                server.update_value(SERVICE, RX)
                await asyncio.sleep(0.01)
        await asyncio.sleep(0.02)


async def watch_link():
    """The link counts as up while the bridge is subscribed to ffe2."""
    was = False
    while True:
        now = await server.is_connected()
        if now and not was:
            lib.shim_link_up(fi)
            log("link up")
        elif was and not now:
            c = (ctypes.c_int * SHIM_COUNTS)()
            lib.shim_counts(fi, c)
            log("link down; connections %d logins %d v0 %d data %d badkey %d timesync %d" % tuple(c[:6]))
        was = now
        await asyncio.sleep(0.2)


async def main():
    global server, loop
    loop = asyncio.get_running_loop()
    adapter = await pick_adapter()
    server = BlessServer(name=NAME, adapter=adapter)
    server.write_request_func = on_write
    server.read_request_func = lambda c, **k: c.value
    await server.add_new_service(SERVICE)
    await server.add_new_characteristic(SERVICE, TX, P.write | P.write_without_response, None, A.writeable)
    await server.add_new_characteristic(SERVICE, RX, P.notify | P.read, None, A.readable)
    # Handlers before start(): Ctrl+C while starting still ends through stop() below.
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    waiter, done = None, set()
    try:
        await server.start()
        log(f"advertising on {adapter}; chunk {CHUNK} B")
        waiter = asyncio.ensure_future(stop.wait())
        tasks = [asyncio.ensure_future(pump()), asyncio.ensure_future(watch_link())]
        done, _ = await asyncio.wait([waiter, *tasks], return_when=asyncio.FIRST_COMPLETED)
        for t in (waiter, *tasks):
            t.cancel()
    finally:
        # Stopping the advertising gives the adapter its own name back (bless set it to NAME),
        # also after a failed start.
        try:
            await server.stop()
            log("stopped")
        except Exception as e:  # noqa: BLE001 - a failed stop must not hide why the fake ended
            log(f"stop failed: {e!r}")
    for t in done:
        if t is not waiter:
            t.result()  # a crash of pump() or watch_link() still ends with its traceback


asyncio.run(main())
