"""HiFlow Pro BLE reader (Hoymiles) — ESPHome external component.

Declares the transport-agnostic parent component. The BLE link, MTU negotiation
and notification plumbing come from the built-in ``ble_client`` component; this
component implements the HiFlow application protocol on top of it (CommCmd
handshake, ``0xA311`` RealDataNew polling with paging, nanopb decoding) and
publishes the decoded values to the sensors in ``sensor/``.

Everything crypto/protocol related lives in the C core under ``src/``; the
flat, ESPHome-loadable copies in this directory are produced by
``./sync_core.sh`` (see README.md).
"""

import os

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker
from esphome.components.zigbee.const import ZigbeeComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAC_ADDRESS, CONF_UPDATE_INTERVAL
from esphome.core import CORE, TimePeriod
from esphome.coroutine import CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor"]

hiflow_ble_ns = cg.esphome_ns.namespace("hiflow_ble")
HiflowBle = hiflow_ble_ns.class_("HiflowBle", cg.Component, ble_client.BLEClientNode)

CONF_HIFLOW_BLE_ID = "hiflow_ble_id"
CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_SN = "sn"
CONF_BLE_ID = "ble_id"
CONF_PIN = "pin"
CONF_OFFSET = "offset"
CONF_EU_DST = "eu_dst"
CONF_POWER_LIMIT = "power_limit"
CONF_INVERTER_CONTROL = "inverter_control"
CONF_NETWORK_TIME = "network_time"
CONF_DEMO_DAY = "demo_day"
CONF_ZIGBEE_ID = "zigbee_id"
CONF_CONNECTION_INTERVAL = "connection_interval"
CONF_NIGHT_SCAN_INTERVAL = "night_scan_interval"
CONF_NIGHT_BLE_OFF = "night_ble_off"
CONF_NIGHT_TEST = "night_test"

# The power limit slider's Zigbee endpoint. Fixed, and well above the sensors
# (1-31), so that adding it never renumbers an existing endpoint.
POWER_LIMIT_ENDPOINT = 32
# The inverter's on/off switch. 33 is the board's uptime sensor in hiflow-zb.yaml.
INVERTER_SWITCH_ENDPOINT = 34
# The Time cluster that reads the network time from the coordinator.
TIME_ENDPOINT = 35

# GATT layout of the HiFlow Pro (see test/ref/hiflow_ble/const.py).
SERVICE_UUID = "0000e0ff-3c17-d293-8e48-14fe2e4da212"
TX_CHAR_UUID16 = 0xFFE1  # app -> device (write)
RX_CHAR_UUID16 = 0xFFE2  # device -> app (notify)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HiflowBle),
        cv.GenerateID(CONF_BLE_CLIENT_ID): cv.use_id(ble_client.BLEClient),
        cv.Required(CONF_SN): cv.string_strict,
        cv.Required(CONF_BLE_ID): cv.string_strict,
        cv.Required(CONF_PIN): cv.string_strict,
        # Standard UTC offset (winter time) used in the handshake time-sync and
        # the protobuf time fields; the vendor app hard-codes 28800 for CST.
        cv.Optional(CONF_OFFSET, default=3600): cv.int_,
        # Add an hour during European summer time. The time-sync (action 104)
        # sets the inverter's own clock, which drives its daily energy reset.
        cv.Optional(CONF_EU_DST, default=True): cv.boolean,
        # Data cadence. The inverter drops an idle link after roughly 90 s, so
        # anything slower than a minute would lose the session between polls.
        cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(seconds=10), max=TimePeriod(seconds=60)),
        ),
        # BLE connection interval once the session is ready. Every connection
        # event wakes the chip; ESP-IDF's default is 30-50 ms. The handshake
        # always runs at the default, and every new connection starts there.
        # Unset keeps the default for the whole connection.
        cv.Optional(CONF_CONNECTION_INTERVAL): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(milliseconds=50), max=TimePeriod(seconds=2)),
        ),
        # Night mode, one of two kinds. The bridge goes to night mode when
        # the link drops after the inverter fed in nothing (standby), or when
        # it has not seen the inverter for 10 minutes after standby or since
        # boot (an hour without either); the first link ends it.
        # night_scan_interval: the scanner only looks this often (the scan
        # window stays the one of esp32_ble_tracker).
        cv.Exclusive(CONF_NIGHT_SCAN_INTERVAL, "night_mode"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(milliseconds=100), max=TimePeriod(milliseconds=10240)),
        ),
        # night_ble_off: BLE off altogether, on for a 15 s scan every
        # night_ble_off; without BLE the 40 MHz crystal and the peripherals
        # can sleep too. BLE must not be shared with another ble_client or a
        # proxy.
        cv.Exclusive(CONF_NIGHT_BLE_OFF, "night_mode"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(seconds=30), max=TimePeriod(minutes=30)),
        ),
        # Bench only: every lost link starts night mode at once, without the
        # standby that a real dusk brings (a fake inverter switched off is
        # then a night).
        cv.Optional(CONF_NIGHT_TEST, default=False): cv.boolean,
        # A slider for the inverter's power limit, as a Zigbee Analog Output on
        # endpoint 32. The bridge only writes when the slider moves. On by
        # default wherever the zigbee component is loaded (see below).
        cv.Optional(CONF_POWER_LIMIT): cv.boolean,
        # A switch that turns the inverter's output on and off, as a Zigbee
        # On/Off cluster on endpoint 34. On by default with Zigbee.
        cv.Optional(CONF_INVERTER_CONTROL): cv.boolean,
        # Read the time from the coordinator (ZHA and Zigbee2MQTT answer from
        # the host's clock) through a Time client cluster on endpoint 35, so
        # the clock is right even after a power cut at night. On by default
        # with Zigbee.
        cv.Optional(CONF_NETWORK_TIME): cv.boolean,
        # For testing the display at night: fills today's day log with a made-up
        # sunny day at boot, never saved to flash. Not for production.
        cv.Optional(CONF_DEMO_DAY, default=False): cv.boolean,
        cv.OnlyWith(CONF_ZIGBEE_ID, "zigbee"): cv.use_id(ZigbeeComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


def _reserve_control_endpoints(config):
    # The controls default to on, but only with Zigbee: the BLE-only build
    # has no endpoint to put them on. Set explicitly, they still need Zigbee.
    config.setdefault(CONF_POWER_LIMIT, CONF_ZIGBEE_ID in config)
    config.setdefault(CONF_INVERTER_CONTROL, CONF_ZIGBEE_ID in config)
    config.setdefault(CONF_NETWORK_TIME, CONF_ZIGBEE_ID in config)
    # Registered with ESPHome's Zigbee component like a sensor endpoint, so the
    # number is taken and the endpoint (basic and identify cluster) is created
    # by its codegen. The control's own cluster is added in to_code.
    from esphome.components.zigbee.const_esp32 import CONF_CLUSTERS, DEVICE_TYPE
    from esphome.components.zigbee.zigbee_ep_esp32 import add_ep

    for option, endpoint in (
        (CONF_POWER_LIMIT, POWER_LIMIT_ENDPOINT),
        (CONF_INVERTER_CONTROL, INVERTER_SWITCH_ENDPOINT),
        (CONF_NETWORK_TIME, TIME_ENDPOINT),
    ):
        if not config[option]:
            continue
        if CONF_ZIGBEE_ID not in config:
            raise cv.Invalid(f"{option} needs the zigbee component")
        add_ep({DEVICE_TYPE: "CUSTOM_ATTR", CONF_CLUSTERS: []}, endpoint, None)
    return config


def _night_options(config):
    if config[CONF_NIGHT_TEST] and not (
        CONF_NIGHT_SCAN_INTERVAL in config or CONF_NIGHT_BLE_OFF in config
    ):
        raise cv.Invalid("night_test needs night_scan_interval or night_ble_off")
    if CONF_NIGHT_BLE_OFF in config:
        # Switching BLE off relies on ESPHome settling the tracker and the
        # client on the way down, which it does from 2026.9.0 on.
        cv.require_esphome_version(2026, 9, 0)(config)
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _night_options, _reserve_control_endpoints)

# The values of hiflow_secrets.example.yaml. A build that still carries one of
# them runs, but never finds the inverter (the MAC) or never logs in (SN,
# ble_id), and nothing on the device says why. CI compiles with the example
# file on purpose and sets HIFLOW_ALLOW_EXAMPLE_SECRETS=1.
EXAMPLE_MAC = "AA:BB:CC:DD:EE:FF"
EXAMPLE_SN = "XXXXXXXXXXXX"
EXAMPLE_BLE_ID = "000000000000000000"
ALLOW_EXAMPLE_ENV = "HIFLOW_ALLOW_EXAMPLE_SECRETS"


def _tracker_scan_params(full_config):
    """The scan_parameters of the esp32_ble_tracker config."""
    tracker = full_config["esp32_ble_tracker"]
    if isinstance(tracker, list):
        tracker = tracker[0]
    return tracker["scan_parameters"]


def _final_validate_night(config):
    full = fv.full_config.get()
    if CONF_NIGHT_SCAN_INTERVAL in config:
        # The stack refuses a window longer than the interval, and the tracker
        # then restarts the scan forever.
        window = _tracker_scan_params(full)["window"]
        if config[CONF_NIGHT_SCAN_INTERVAL].total_milliseconds < window.total_milliseconds:
            raise cv.Invalid(
                f"night_scan_interval must not be shorter than the tracker's scan window ({window})"
            )
    if CONF_NIGHT_BLE_OFF in config:
        # BLE off at night takes it from every other BLE user too.
        others = [
            client[CONF_ID]
            for client in full.get("ble_client", [])
            if client[CONF_ID] != config[CONF_BLE_CLIENT_ID]
        ]
        if others or "bluetooth_proxy" in full:
            raise cv.Invalid(
                "night_ble_off switches BLE off for everything; it cannot share it "
                "with another ble_client or bluetooth_proxy"
            )
    return config


def _final_validate_secrets(config):
    if os.environ.get(ALLOW_EXAMPLE_ENV) == "1":
        return config
    left = []
    if config[CONF_SN] == EXAMPLE_SN:
        left.append("hiflow_sn")
    if config[CONF_BLE_ID] == EXAMPLE_BLE_ID:
        left.append("hiflow_ble_id")
    for client in fv.full_config.get().get("ble_client", []):
        if client[CONF_ID] == config[CONF_BLE_CLIENT_ID] and str(
            client[CONF_MAC_ADDRESS]
        ) == EXAMPLE_MAC:
            left.append("hiflow_mac")
    if left:
        raise cv.Invalid(
            f"hiflow_secrets.yaml still holds the example value of {', '.join(left)}; "
            f"fill in your inverter's values (set {ALLOW_EXAMPLE_ENV}=1 to build anyway)"
        )
    return config


FINAL_VALIDATE_SCHEMA = cv.All(_final_validate_night, _final_validate_secrets)


async def to_code(config):
    # Select the mbedTLS crypto backend of the C core. Defines survive
    # cg.add_build_flag(); -I flags do not, which is why sync_core.sh flattens
    # the sources into this directory instead of using an include path.
    cg.add_build_flag("-DHIFLOW_CRYPTO_BACKEND_MBEDTLS")

    # Not a PollingComponent: the session core decides when to poll, so the
    # interval must not reach register_component (which would look for a
    # set_update_interval()).
    poll_interval = config.pop(CONF_UPDATE_INTERVAL)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_sn(config[CONF_SN]))
    cg.add(var.set_ble_id(config[CONF_BLE_ID]))
    cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_offset(config[CONF_OFFSET]))
    cg.add(var.set_eu_dst(config[CONF_EU_DST]))
    cg.add(var.set_poll_interval(poll_interval))
    if CONF_CONNECTION_INTERVAL in config:
        cg.add(var.set_connection_interval(config[CONF_CONNECTION_INTERVAL]))
    if config[CONF_NIGHT_TEST]:
        cg.add(var.set_night_test(True))
    if CONF_NIGHT_BLE_OFF in config:
        cg.add(var.set_night_ble_off(config[CONF_NIGHT_BLE_OFF]))
    if CONF_NIGHT_SCAN_INTERVAL in config:
        # The day values come from the tracker's own config (0.625 ms units),
        # so night mode can go back to them.
        scan = _tracker_scan_params(CORE.config)
        cg.add(
            var.set_night_scan(
                int(config[CONF_NIGHT_SCAN_INTERVAL].total_milliseconds / 0.625),
                int(scan["interval"].total_milliseconds / 0.625),
                int(scan["window"].total_milliseconds / 0.625),
            )
        )
    cg.add(
        var.set_service_uuid128(
            esp32_ble_tracker.as_reversed_hex_array(SERVICE_UUID)
        )
    )
    cg.add(var.set_tx_uuid16(TX_CHAR_UUID16))
    cg.add(var.set_rx_uuid16(RX_CHAR_UUID16))

    if config[CONF_POWER_LIMIT]:
        zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
        cg.add(var.set_power_limit_slider(zb, POWER_LIMIT_ENDPOINT))
        CORE.add_job(_add_power_limit_cluster, zb)

    if config[CONF_INVERTER_CONTROL]:
        zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
        cg.add(var.set_inverter_switch(zb, INVERTER_SWITCH_ENDPOINT))
        CORE.add_job(_add_inverter_switch_cluster, zb)

    if config[CONF_DEMO_DAY]:
        cg.add_define("HIFLOW_DEMO_DAY")

    if config[CONF_NETWORK_TIME]:
        zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
        cg.add(var.set_network_time(zb, TIME_ENDPOINT))
        CORE.add_job(_add_time_cluster, zb)


# The clusters go onto the endpoints that the Zigbee codegen creates, so they have
# to come after that code in setup(), and before App.setup() starts the Zigbee
# task that registers the device.
@coroutine_with_priority(CoroPriority.LATE)
async def _add_power_limit_cluster(zb):
    cg.add(hiflow_ble_ns.add_power_limit_cluster(zb, POWER_LIMIT_ENDPOINT))


@coroutine_with_priority(CoroPriority.LATE)
async def _add_inverter_switch_cluster(zb):
    cg.add(hiflow_ble_ns.add_inverter_switch_cluster(zb, INVERTER_SWITCH_ENDPOINT))


@coroutine_with_priority(CoroPriority.LATE)
async def _add_time_cluster(zb):
    cg.add(hiflow_ble_ns.add_time_cluster(zb, TIME_ENDPOINT))
