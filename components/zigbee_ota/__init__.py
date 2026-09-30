"""Firmware updates over Zigbee (ZCL OTA Upgrade client) for ESPHome's zigbee
component on the ESP32-C6.

ESPHome's zigbee component has no OTA. This adds the OTA Upgrade client cluster
(0x0019) of esp-zigbee-lib to one endpoint, writes the received image into the
free app slot and boots it. The coordinator acts as the OTA server and serves
a file made by tools/make_zigbee_ota.py (tested with ZHA; Zigbee2MQTT not yet).

The file version is the Unix time of the code generation, written together
with manufacturer code and image type to zigbee_ota.json in the build
directory, where the packer takes them from. Every compile gets a newer
version, so the image in the file and the version it claims cannot disagree.
(ESPHome's own build time in build_info.json is not used: it stays the same
across some config changes.)

App rollback: the bootloader boots a new image in "pending verify" state. The
image only counts as good once the OTA server answered it (a Query Next Image
round trip); an image that crashes before that, or cannot reach the
coordinator within `verify_timeout`, falls back to the previous image. The
rollback option lives in the bootloader, which only a USB flash writes: the
first image with this component has to go on by cable.
"""

import json
import time

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.components.zigbee.const import ZigbeeComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority

DEPENDENCIES = ["zigbee"]
CODEOWNERS = []

CONF_ZIGBEE_ID = "zigbee_id"
CONF_ENDPOINT = "endpoint"
CONF_MANUFACTURER_CODE = "manufacturer_code"
CONF_IMAGE_TYPE = "image_type"
CONF_BLOCK_SIZE = "block_size"
CONF_VERIFY_TIMEOUT = "verify_timeout"
CONF_TX_POWER = "tx_power"

zigbee_ota_ns = cg.esphome_ns.namespace("zigbee_ota")
ZigbeeOta = zigbee_ota_ns.class_("ZigbeeOta", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ZigbeeOta),
        cv.GenerateID(CONF_ZIGBEE_ID): cv.use_id(ZigbeeComponent),
        # Any endpoint that ESPHome's zigbee codegen creates; 1 always exists
        # once there is one Zigbee sensor.
        cv.Optional(CONF_ENDPOINT, default=1): cv.int_range(min=1, max=240),
        # Espressif's code, the one esp-zigbee-sdk's examples use. zigpy caps
        # the block size by this code (50 bytes for everyone but a few vendors).
        cv.Optional(CONF_MANUFACTURER_CODE, default=0x131B): cv.hex_uint16_t,
        # Tells this firmware apart from other Espressif-code images in the same
        # OTA folder. "HF".
        cv.Optional(CONF_IMAGE_TYPE, default=0x4846): cv.hex_uint16_t,
        # Largest block the client asks for. The server may send less.
        cv.Optional(CONF_BLOCK_SIZE, default=223): cv.int_range(min=16, max=254),
        cv.Optional(
            CONF_VERIFY_TIMEOUT, default="10min"
        ): cv.positive_time_period_milliseconds,
        # Radio TX power in dBm once the stack runs; unset leaves the stack's
        # default (20 dBm). A board on a weak supply that browns out during a
        # download (flash writes on top of full-power TX) can go lower.
        cv.Optional(CONF_TX_POWER): cv.int_range(min=-24, max=20),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_zigbee(zb))
    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_manufacturer_code(config[CONF_MANUFACTURER_CODE]))
    cg.add(var.set_image_type(config[CONF_IMAGE_TYPE]))
    cg.add(var.set_block_size(config[CONF_BLOCK_SIZE]))
    cg.add(var.set_verify_timeout(config[CONF_VERIFY_TIMEOUT]))

    version = int(time.time())
    cg.add(var.set_file_version(version))
    info = CORE.relative_build_path("zigbee_ota.json")
    info.parent.mkdir(parents=True, exist_ok=True)
    info.write_text(
        json.dumps(
            {
                "file_version": version,
                "manufacturer_code": config[CONF_MANUFACTURER_CODE],
                "image_type": config[CONF_IMAGE_TYPE],
            },
            indent=2,
        )
        + "\n"
    )
    if CONF_TX_POWER in config:
        cg.add(var.set_tx_power(config[CONF_TX_POWER]))

    # Bootloader app rollback. ESPHome only turns it on together with its own
    # ota + safe_mode; USE_OTA_ROLLBACK keeps esp32/hal.cpp from marking the
    # running image valid at boot, this component does that instead.
    add_idf_sdkconfig_option("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE", True)
    cg.add_define("USE_OTA_ROLLBACK")

    # Delta images (tools/make_zigbee_ota.py --from): detools patches,
    # applied against the running app partition.
    add_idf_component(name="espressif/esp_delta_ota", ref="1.1.4")

    CORE.add_job(_add_cluster, var)


# The cluster goes onto an endpoint that the zigbee codegen creates, so it has
# to come after that code in setup(), and before App.setup() starts the Zigbee
# task that registers the device.
@coroutine_with_priority(CoroPriority.LATE)
async def _add_cluster(var):
    cg.add(var.add_cluster())
