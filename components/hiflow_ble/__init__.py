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
CONF_ZIGBEE_ID = "zigbee_id"

# The power limit slider's Zigbee endpoint. Fixed, and well above the sensors
# (1-31), so that adding it never renumbers an existing endpoint.
POWER_LIMIT_ENDPOINT = 32
# The inverter's on/off switch. 33 is the board's uptime sensor in esp32c6.yaml.
INVERTER_SWITCH_ENDPOINT = 34

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
        # A slider for the inverter's power limit, as a Zigbee Analog Output on
        # endpoint 32. The bridge only writes when the slider moves. On by
        # default wherever the zigbee component is loaded (see below).
        cv.Optional(CONF_POWER_LIMIT): cv.boolean,
        # A switch that turns the inverter's output on and off, as a Zigbee
        # On/Off cluster on endpoint 34. Off unless asked for.
        cv.Optional(CONF_INVERTER_CONTROL, default=False): cv.boolean,
        cv.OnlyWith(CONF_ZIGBEE_ID, "zigbee"): cv.use_id(ZigbeeComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


def _reserve_control_endpoints(config):
    # The power limit defaults to on, but only with Zigbee: the BLE-only build
    # has no endpoint to put it on. Set explicitly, it still needs Zigbee.
    config.setdefault(CONF_POWER_LIMIT, CONF_ZIGBEE_ID in config)
    # Registered with ESPHome's Zigbee component like a sensor endpoint, so the
    # number is taken and the endpoint (basic and identify cluster) is created
    # by its codegen. The control's own cluster is added in to_code.
    from esphome.components.zigbee.const_esp32 import CONF_CLUSTERS, DEVICE_TYPE
    from esphome.components.zigbee.zigbee_ep_esp32 import add_ep

    for option, endpoint in (
        (CONF_POWER_LIMIT, POWER_LIMIT_ENDPOINT),
        (CONF_INVERTER_CONTROL, INVERTER_SWITCH_ENDPOINT),
    ):
        if not config[option]:
            continue
        if CONF_ZIGBEE_ID not in config:
            raise cv.Invalid(f"{option} needs the zigbee component")
        add_ep({DEVICE_TYPE: "CUSTOM_ATTR", CONF_CLUSTERS: []}, endpoint, None)
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _reserve_control_endpoints)

# The values of hiflow_secrets.example.yaml. A build that still carries one of
# them runs, but never finds the inverter (the MAC) or never logs in (SN,
# ble_id), and nothing on the device says why. CI compiles with the example
# file on purpose and sets HIFLOW_ALLOW_EXAMPLE_SECRETS=1.
EXAMPLE_MAC = "AA:BB:CC:DD:EE:FF"
EXAMPLE_SN = "XXXXXXXXXXXX"
EXAMPLE_BLE_ID = "000000000000000000"
ALLOW_EXAMPLE_ENV = "HIFLOW_ALLOW_EXAMPLE_SECRETS"


def _final_validate(config):
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


FINAL_VALIDATE_SCHEMA = _final_validate


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


# The clusters go onto the endpoints that the Zigbee codegen creates, so they have
# to come after that code in setup(), and before App.setup() starts the Zigbee
# task that registers the device.
@coroutine_with_priority(CoroPriority.LATE)
async def _add_power_limit_cluster(zb):
    cg.add(hiflow_ble_ns.add_power_limit_cluster(zb, POWER_LIMIT_ENDPOINT))


@coroutine_with_priority(CoroPriority.LATE)
async def _add_inverter_switch_cluster(zb):
    cg.add(hiflow_ble_ns.add_inverter_switch_cluster(zb, INVERTER_SWITCH_ENDPOINT))
