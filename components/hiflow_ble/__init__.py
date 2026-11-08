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

import logging
import os

import esphome.codegen as cg
from esphome.components import (
    ble_client,
    esp32_ble_tracker,
    number,
    switch,
    time as time_,
)
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.zigbee.const import (
    CONF_CLUSTER,
    CONF_ENDPOINT,
    CONF_REPORT,
    CONF_USE_DEVICE_TYPE,
    ZigbeeComponent,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_NAME,
    CONF_RESTORE_MODE,
    CONF_TIME_ID,
    CONF_UPDATE_INTERVAL,
    UNIT_PERCENT,
)
from esphome.core import CORE, TimePeriod
from esphome.coroutine import CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

DEPENDENCIES = ["ble_client"]

_LOGGER = logging.getLogger(__name__)

hiflow_ble_ns = cg.esphome_ns.namespace("hiflow_ble")
HiflowBle = hiflow_ble_ns.class_("HiflowBle", cg.Component, ble_client.BLEClientNode)
HiflowPowerLimit = hiflow_ble_ns.class_(
    "HiflowPowerLimit", number.Number, cg.Parented.template(HiflowBle)
)
HiflowInverterSwitch = hiflow_ble_ns.class_(
    "HiflowInverterSwitch", switch.Switch, cg.Parented.template(HiflowBle)
)

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
CONF_BTU_TASK_STACK_SIZE = "btu_task_stack_size"

# The power limit slider's Zigbee endpoint. Fixed, and well above the sensors
# (1-31), so that adding it never renumbers an existing endpoint.
POWER_LIMIT_ENDPOINT = 32
# The inverter's on/off switch. 33 is the board's uptime sensor in hiflow-zb.yaml.
INVERTER_SWITCH_ENDPOINT = 34



def AUTO_LOAD(config):
    # number and switch only for the controls that are on (see
    # _control_entities). A falsy config is ESPHome asking for the full set.
    loads = ["sensor"]
    if not config or config.get(CONF_POWER_LIMIT):
        loads.append("number")
    if not config or config.get(CONF_INVERTER_CONTROL):
        loads.append("switch")
    return loads


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
        # Standard UTC offset (winter time) for the display and the day curve.
        # The handshake time-sync and the protobuf time fields carry it too;
        # the vendor app hard-codes 28800 for CST.
        cv.Optional(CONF_OFFSET, default=3600): cv.int_,
        # Add an hour during European summer time (display, day curve,
        # time-sync).
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
        # A slider for the inverter's power limit: a number entity, which
        # ESPHome's Zigbee component puts on an Analog Output on endpoint 32.
        # The bridge only writes when the slider moves. On by default wherever
        # the zigbee component is loaded (see below).
        cv.Optional(CONF_POWER_LIMIT): cv.boolean,
        # A switch that turns the inverter's output on and off: a switch
        # entity on a Zigbee On/Off cluster on endpoint 34. On by default with
        # Zigbee.
        cv.Optional(CONF_INVERTER_CONTROL): cv.boolean,
        # The network time: every sync of this time component sets the
        # bridge's clock, so it is right even after a power cut at night.
        # hiflow-zb.yaml points it at ESPHome's zigbee time platform, which
        # reads the coordinator's clock (ZHA and Zigbee2MQTT answer from the
        # host's clock).
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_NETWORK_TIME): cv.invalid(
            "network_time was replaced by time_id: add a `time:` entry with "
            "`platform: zigbee` and point time_id at it (see hiflow-zb.yaml); "
            "to go without a network time, delete the line"
        ),
        # For testing the display at night: fills today's day log with a made-up
        # sunny day at boot, never saved to flash. Not for production.
        cv.Optional(CONF_DEMO_DAY, default=False): cv.boolean,
        # Stack of Bluedroid's BTU task, in bytes. esp32_ble_tracker sets 8192
        # (to match Arduino); the ESP-IDF default is 4096. A board short of RAM
        # (the ESP32-H2) sets it here, because sdkconfig_options cannot
        # override the tracker's value.
        cv.Optional(CONF_BTU_TASK_STACK_SIZE): cv.int_range(min=3072, max=8192),
        cv.OnlyWith(CONF_ZIGBEE_ID, "zigbee"): cv.use_id(ZigbeeComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


# The controls are ordinary number and switch entities. ESPHome's Zigbee
# component gives each its endpoint and cluster and hands every write from the
# coordinator to the entity. Their names stay inside the firmware, except that
# the slider's goes out as the Analog Output's description. `report: force`
# sends each new value straight to the coordinator, like the sensors.
POWER_LIMIT_SCHEMA = number.number_schema(HiflowPowerLimit, unit_of_measurement=UNIT_PERCENT)
POWER_LIMIT_ENTITY = {
    CONF_NAME: "HiFlow Power Limit",
    CONF_ENDPOINT: POWER_LIMIT_ENDPOINT,
    CONF_REPORT: "force",
}
INVERTER_SWITCH_SCHEMA = switch.switch_schema(HiflowInverterSwitch, block_inverted=True)
INVERTER_SWITCH_ENTITY = {
    CONF_NAME: "HiFlow Inverter",
    CONF_ENDPOINT: INVERTER_SWITCH_ENDPOINT,
    # On/Off, as before; ESPHome's default for a switch is a Binary Output.
    CONF_CLUSTER: "on_off",
    CONF_REPORT: "force",
    # The endpoint keeps the device type it had before the entity (custom
    # attributes): a new one would show up only after a new interview.
    CONF_USE_DEVICE_TYPE: False,
    # The bridge restores the last confirmed state itself (see hiflow_ble.cpp).
    CONF_RESTORE_MODE: "DISABLED",
}


def _control_entities(config):
    # The controls default to on, but only with Zigbee: the BLE-only build
    # has no endpoint to put them on. Set explicitly, they still need Zigbee.
    # An enabled control becomes the validated entity config (validating it
    # registers its endpoint with the Zigbee component); a disabled one goes.
    for option, schema, entity in (
        (CONF_POWER_LIMIT, POWER_LIMIT_SCHEMA, POWER_LIMIT_ENTITY),
        (CONF_INVERTER_CONTROL, INVERTER_SWITCH_SCHEMA, INVERTER_SWITCH_ENTITY),
    ):
        if not config.get(option, CONF_ZIGBEE_ID in config):
            config.pop(option, None)
            continue
        if CONF_ZIGBEE_ID not in config:
            raise cv.Invalid(f"{option} needs the zigbee component")
        config[option] = schema(dict(entity))
    return config


def _time_hint(config):
    # Until the zigbee time platform, the bridge read the network time by
    # itself whenever Zigbee was on. An own config from that time now builds
    # without it and says nothing.
    if CONF_ZIGBEE_ID in config and CONF_TIME_ID not in config:
        _LOGGER.warning(
            "hiflow_ble: no time_id, so no network time: the inverter's time sets "
            "the bridge's clock. hiflow-zb.yaml adds a `time:` entry with "
            "`platform: zigbee` and points time_id at it."
        )
    return config


def _night_options(config):
    if config[CONF_NIGHT_TEST] and not (
        CONF_NIGHT_SCAN_INTERVAL in config or CONF_NIGHT_BLE_OFF in config
    ):
        raise cv.Invalid("night_test needs night_scan_interval or night_ble_off")
    return config


# night_ble_off relies on ESPHome settling the tracker and the client when BLE
# goes off; the bridge is built and tested on 2026.10 and newer only.
CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA,
    cv.require_esphome_version(2026, 10, 0),
    _night_options,
    _control_entities,
    _time_hint,
)

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

    if limit_config := config.get(CONF_POWER_LIMIT):
        # The inverter takes 10 % steps only; ZHA takes the step from the
        # Analog Output's resolution.
        limit = await number.new_number(limit_config, min_value=0, max_value=100, step=10)
        cg.add(limit.set_parent(var))
        cg.add(var.set_power_limit(limit))
        CORE.add_job(_add_power_limit_range, await cg.get_variable(config[CONF_ZIGBEE_ID]))

    if switch_config := config.get(CONF_INVERTER_CONTROL):
        inverter_switch = await switch.new_switch(switch_config)
        cg.add(inverter_switch.set_parent(var))
        cg.add(var.set_inverter_switch(inverter_switch))

    if config[CONF_DEMO_DAY]:
        cg.add_define("HIFLOW_DEMO_DAY")

    # The day's curve is only drawn by a display (see hiflow_daylog.h). A
    # build flag, not a define: the C core has to see it too.
    if "display" not in CORE.config:
        cg.add_build_flag("-DHIFLOW_DAYLOG_NO_CURVE")

    if CONF_BTU_TASK_STACK_SIZE in config:
        CORE.add_job(_set_btu_task_stack_size, config[CONF_BTU_TASK_STACK_SIZE])

    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_network_time(clock))


# ESPHome's Zigbee number on the ESP32 gives the Analog Output no range,
# resolution or application type. Without them ZHA shows a slider from 0 to
# 1023, and Zigbee2MQTT names the value differently. These go onto the
# cluster that the zigbee codegen creates for endpoint 32, so after that code
# in setup() and before App.setup() registers the device.
POWER_LIMIT_ATTRIBUTES = (
    (0x0041, cg.float_, 100.0),  # MaxPresentValue
    (0x0045, cg.float_, 0.0),  # MinPresentValue
    (0x006A, cg.float_, 10.0),  # Resolution
    # ApplicationType: group Analog Output (0x01), type Percentage (0x04),
    # index 0xFFFF (vendor defined)
    (0x0100, cg.uint32, 0x0104FFFF),
)


@coroutine_with_priority(CoroPriority.LATE)
async def _add_power_limit_range(zb):
    for attr_id, c_type, value in POWER_LIMIT_ATTRIBUTES:
        cg.add(
            zb.add_attr(
                cg.TemplateArguments(c_type),
                POWER_LIMIT_ENDPOINT,
                cg.RawExpression("::EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT"),
                cg.RawExpression("EZB_ZCL_CLUSTER_SERVER"),
                attr_id,
                0,
                value,
            )
        )


# After esp32_ble_tracker's to_code, which sets its own value.
@coroutine_with_priority(CoroPriority.FINAL)
async def _set_btu_task_stack_size(size):
    add_idf_sdkconfig_option("CONFIG_BT_BTU_TASK_STACK_SIZE", size)
