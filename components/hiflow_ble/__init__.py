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

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.core import TimePeriod

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
    }
).extend(cv.COMPONENT_SCHEMA)


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
