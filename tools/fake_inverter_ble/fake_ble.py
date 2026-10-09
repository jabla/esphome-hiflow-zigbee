#!/usr/bin/env python3
"""BLE peripheral that plays a HiFlow Pro: GATT service e0ff with ffe1 (write)
and ffe2 (notify); every written frame goes to fake_inverter.c (libfake.so),
the reply goes back as notifications. Bench use only, see README.md."""

import asyncio
import ctypes
import json
import os
from pathlib import Path
import time

from bless import BlessServer
from bless import GATTAttributePermissions as A
from bless import GATTCharacteristicProperties as P
from dbus_next import BusType
from dbus_next.aio import MessageBus

HERE = Path(__file__).resolve().parent
SERVICE = "0000e0ff-3c17-d293-8e48-14fe2e4da212"
TX = "0000ffe1-0000-1000-8000-00805f9b34fb"  # bridge writes
RX = "0000ffe2-0000-1000-8000-00805f9b34fb"  # bridge gets notified
CHUNK = int(os.environ.get("CHUNK", "20"))
KEY = bytes.fromhex(os.environ.get("KEY", "00112233445566778899aabbccddeeff"))
SN = os.environ.get("SN", "0000000000AA").encode()
PIN = os.environ.get("PIN", "").encode()
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
lib.shim_set_power.argtypes = [ctypes.c_void_p, ctypes.c_int32]
lib.shim_reset_pages.argtypes = [ctypes.c_void_p]
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


async def drop_link():
    """The fake drops the link: disconnect the one connected central."""
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    intro = await bus.introspect("org.bluez", "/")
    manager = bus.get_proxy_object("org.bluez", "/", intro).get_interface("org.freedesktop.DBus.ObjectManager")
    objects = await manager.call_get_managed_objects()
    connected = [
        path for path, ifaces in objects.items()
        if "org.bluez.Device1" in ifaces and ifaces["org.bluez.Device1"]["Connected"].value
    ]
    if len(connected) != 1:
        log(f"KILL: {len(connected)} devices connected, not dropping any")
        return
    intro = await bus.introspect("org.bluez", connected[0])
    device = bus.get_proxy_object("org.bluez", connected[0], intro).get_interface("org.bluez.Device1")
    await device.call_disconnect()
    log("KILL: link dropped")


def on_write(characteristic, value, **kwargs):
    lib.shim_set_time(fi, int(time.time()))
    apply_power_file()
    out = ctypes.create_string_buffer(2048)
    kill = ctypes.c_int(0)
    n = lib.shim_frame(fi, bytes(value), len(value), out, ctypes.byref(kill))
    log(f"<- {len(value)} B, -> {n} B{' KILL' if kill.value else ''}")
    if n:
        pending.append(out.raw[:n])
    if kill.value:
        loop.call_soon_threadsafe(lambda: asyncio.ensure_future(drop_link()))


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
            c = (ctypes.c_int * 6)()
            lib.shim_counts(fi, c)
            log("link down; connections %d logins %d v0 %d data %d badkey %d timesync %d" % tuple(c))
        was = now
        await asyncio.sleep(0.2)


async def main():
    global server, loop
    loop = asyncio.get_running_loop()
    server = BlessServer(name="HiFlowFake")
    server.write_request_func = on_write
    server.read_request_func = lambda c, **k: c.value
    await server.add_new_service(SERVICE)
    await server.add_new_characteristic(SERVICE, TX, P.write | P.write_without_response, None, A.writeable)
    await server.add_new_characteristic(SERVICE, RX, P.notify | P.read, None, A.readable)
    await server.start()
    log(f"advertising; chunk {CHUNK} B")
    await asyncio.gather(pump(), watch_link())


asyncio.run(main())
