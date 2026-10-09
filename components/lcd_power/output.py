from esphome import pins
import esphome.codegen as cg
from esphome.components import output
from esphome.components.esp32 import include_builtin_idf_component
import esphome.config_validation as cv
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_PIN

DEPENDENCIES = ["esp32"]

lcd_power_ns = cg.esphome_ns.namespace("lcd_power")
BacklightOutput = lcd_power_ns.class_("BacklightOutput", output.FloatOutput, cg.Component)

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(BacklightOutput),
        cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
        # RC_FAST gives 9 bits of duty at 20 kHz on the C6, 8 bits on the H2.
        cv.Optional(CONF_FREQUENCY, default="20kHz"): cv.All(
            cv.frequency, cv.float_range(min=100, max=100000)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    cg.add(var.set_frequency(int(config[CONF_FREQUENCY])))
    # set_status_led() uses the RMT driver, which ESPHome leaves out of the
    # build unless a component asks for it.
    include_builtin_idf_component("esp_driver_rmt")
