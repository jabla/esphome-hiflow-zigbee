"""Sensor platform of the ``hiflow_ble`` component.

One entity per measurement. ``type:`` picks which decoded HiFlow value the
entity is bound to; the unit / device class / state class default to the
physically correct ones and can still be overridden in YAML.
"""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_TYPE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_VOLT_AMPS_REACTIVE,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)

from .. import CONF_HIFLOW_BLE_ID, HiflowBle, hiflow_ble_ns

DEPENDENCIES = ["hiflow_ble"]

HiflowSensor = hiflow_ble_ns.class_("HiflowSensor", sensor.Sensor, cg.Component)
HiflowSensorType = hiflow_ble_ns.enum("HiflowSensorType")

_POWER = dict(
    unit_of_measurement=UNIT_WATT,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
)
_VOLTAGE = dict(
    unit_of_measurement=UNIT_VOLT,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
)
_CURRENT = dict(
    unit_of_measurement=UNIT_AMPERE,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=2,
)
_FREQUENCY = dict(
    unit_of_measurement=UNIT_HERTZ,
    device_class=DEVICE_CLASS_FREQUENCY,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=2,
)
_TEMPERATURE = dict(
    unit_of_measurement=UNIT_CELSIUS,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
)
_ENERGY_TOTAL = dict(
    unit_of_measurement=UNIT_WATT_HOURS,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    accuracy_decimals=0,
)
_STATUS = dict(
    # State code of the BLE session; no device_class, so ZHA does not read it
    # as a physical quantity.
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=0,
)
_REACTIVE_POWER = dict(
    unit_of_measurement=UNIT_VOLT_AMPS_REACTIVE,
    device_class=DEVICE_CLASS_REACTIVE_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
)
_POWER_FACTOR = dict(
    unit_of_measurement=UNIT_PERCENT,
    device_class=DEVICE_CLASS_POWER_FACTOR,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
)
_WARNINGS = dict(
    # The inverter's own counter of warnings since dawn; a count, no unit.
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=0,
)
_ENERGY_DAILY = dict(
    unit_of_measurement=UNIT_WATT_HOURS,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=0,
)


def _enum(name):
    return getattr(HiflowSensorType, name)


# yaml `type:` -> (C++ enum constant, default entity metadata)
# Order of the port entries mirrors the HiflowSensorType enum.
SENSOR_TYPES = {
    "ac_power": (_enum("HIFLOW_AC_POWER"), _POWER),
    "ac_voltage": (_enum("HIFLOW_AC_VOLTAGE"), _VOLTAGE),
    "ac_current": (_enum("HIFLOW_AC_CURRENT"), _CURRENT),
    "ac_frequency": (_enum("HIFLOW_AC_FREQUENCY"), _FREQUENCY),
    "temperature": (_enum("HIFLOW_TEMPERATURE"), _TEMPERATURE),
    "energy_total": (_enum("HIFLOW_ENERGY_TOTAL"), _ENERGY_TOTAL),
    "energy_daily": (_enum("HIFLOW_ENERGY_DAILY"), _ENERGY_DAILY),
    "port1_power": (_enum("HIFLOW_PORT1_POWER"), _POWER),
    "port1_voltage": (_enum("HIFLOW_PORT1_VOLTAGE"), _VOLTAGE),
    "port1_current": (_enum("HIFLOW_PORT1_CURRENT"), _CURRENT),
    "port2_power": (_enum("HIFLOW_PORT2_POWER"), _POWER),
    "port2_voltage": (_enum("HIFLOW_PORT2_VOLTAGE"), _VOLTAGE),
    "port2_current": (_enum("HIFLOW_PORT2_CURRENT"), _CURRENT),
    "port3_power": (_enum("HIFLOW_PORT3_POWER"), _POWER),
    "port3_voltage": (_enum("HIFLOW_PORT3_VOLTAGE"), _VOLTAGE),
    "port3_current": (_enum("HIFLOW_PORT3_CURRENT"), _CURRENT),
    "port4_power": (_enum("HIFLOW_PORT4_POWER"), _POWER),
    "port4_voltage": (_enum("HIFLOW_PORT4_VOLTAGE"), _VOLTAGE),
    "port4_current": (_enum("HIFLOW_PORT4_CURRENT"), _CURRENT),
    "status": (_enum("HIFLOW_STATUS"), _STATUS),
    "reactive_power": (_enum("HIFLOW_REACTIVE_POWER"), _REACTIVE_POWER),
    "power_factor": (_enum("HIFLOW_POWER_FACTOR"), _POWER_FACTOR),
    "warnings": (_enum("HIFLOW_WARNINGS"), _WARNINGS),
    "port1_energy_total": (_enum("HIFLOW_PORT1_ENERGY_TOTAL"), _ENERGY_TOTAL),
    "port1_energy_daily": (_enum("HIFLOW_PORT1_ENERGY_DAILY"), _ENERGY_DAILY),
    "port2_energy_total": (_enum("HIFLOW_PORT2_ENERGY_TOTAL"), _ENERGY_TOTAL),
    "port2_energy_daily": (_enum("HIFLOW_PORT2_ENERGY_DAILY"), _ENERGY_DAILY),
    "port3_energy_total": (_enum("HIFLOW_PORT3_ENERGY_TOTAL"), _ENERGY_TOTAL),
    "port3_energy_daily": (_enum("HIFLOW_PORT3_ENERGY_DAILY"), _ENERGY_DAILY),
    "port4_energy_total": (_enum("HIFLOW_PORT4_ENERGY_TOTAL"), _ENERGY_TOTAL),
    "port4_energy_daily": (_enum("HIFLOW_PORT4_ENERGY_DAILY"), _ENERGY_DAILY),
}


def _apply_type_defaults(config):
    """Fill in the physical metadata for the chosen type (YAML still wins)."""
    if CONF_TYPE not in config:
        return config  # let the schema below report the proper error
    _, defaults = SENSOR_TYPES[config[CONF_TYPE]]
    for key, value in defaults.items():
        config.setdefault(key, value)
    return config


CONFIG_SCHEMA = cv.All(
    _apply_type_defaults,
    sensor.sensor_schema(HiflowSensor, accuracy_decimals=1).extend(
        {
            cv.GenerateID(CONF_HIFLOW_BLE_ID): cv.use_id(HiflowBle),
            cv.Required(CONF_TYPE): cv.one_of(*SENSOR_TYPES, lower=True),
        }
    ),
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_HIFLOW_BLE_ID])
    cg.add(var.set_parent(parent))
    enum_value, _ = SENSOR_TYPES[config[CONF_TYPE]]
    cg.add(var.set_type(enum_value))
