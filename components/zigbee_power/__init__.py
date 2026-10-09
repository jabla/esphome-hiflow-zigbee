"""Power saving for ESPHome's zigbee component on the ESP32-C6 and ESP32-H2.

With `sleepy: true` the node becomes a sleepy end device (receiver off while
idle, it polls its parent every `poll_interval`) and the chip goes to light
sleep whenever every task is idle (ESP-IDF power management with tickless
idle), with the 802.15.4 radio and the BLE controller sleeping as well. The
parent holds messages for a sleepy child for about 7.5 s, so the poll
interval has to stay below that or commands from the coordinator get lost.
After a request sent to the node it polls every 50 ms for
`request_poll_window`, so an interview does not wait a poll per request.
`loop_interval` also works without it.

The light-sleep callbacks of ESP-IDF count the time actually slept; `slept_ms()`
gives the running total, so a template sensor can report the share of time
asleep without any measuring equipment.

The parent learns the receiver mode at the join only. This component keeps
the mode of the join in flash; after switching `sleepy` the node joins again
by itself once the running image is confirmed (the coordinator must permit
joins then). A board that ran a sleepy image and then gets an image without
this component, a rollback included, keeps its receiver off until it is
paired again.

GPIO outputs float while the chip sleeps (see to_code below); `keep_pins`
keeps their level.

In light sleep the USB port goes down, so a board on a laptop drops off it.
For `awake_after_boot` after a start on USB the chip stays awake (logs, a USB
flash after a press on RESET); without a USB host it sleeps at once. Updates
without touching the board go over Zigbee OTA.

Two workarounds for faults below ESPHome: phy_guard (sleepy only, see
phy_guard.h) and on the ESP32-C6 timer_guard (`timer_guard`, see
timer_guard.h).
"""

import logging

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    add_idf_sdkconfig_option,
    get_esp32_variant,
    include_builtin_idf_component,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_PRESS, CONF_PIN, CONF_TRIGGER_ID
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["zigbee"]
CODEOWNERS = []

CONF_SLEEPY = "sleepy"
CONF_POLL_INTERVAL = "poll_interval"
CONF_AWAKE_AFTER_BOOT = "awake_after_boot"
CONF_LOOP_INTERVAL = "loop_interval"
CONF_NIGHT_POLL_INTERVAL = "night_poll_interval"
CONF_NIGHT_LOOP_INTERVAL = "night_loop_interval"
CONF_POWER_DOWN_PERIPHERALS = "power_down_peripherals"
CONF_SLEEP_DEBUG = "sleep_debug"
CONF_BLE_SLEEP_CLOCK = "ble_sleep_clock"
CONF_WAKE_BUTTON = "wake_button"
CONF_KEEP_PINS = "keep_pins"
CONF_REQUEST_POLL_WINDOW = "request_poll_window"
CONF_TIMER_GUARD = "timer_guard"

# Options that only act on a sleepy end device, with their defaults.
SLEEPY_DEFAULTS = {
    CONF_POLL_INTERVAL: "3s",
    CONF_AWAKE_AFTER_BOOT: "3min",
    CONF_POWER_DOWN_PERIPHERALS: False,
    CONF_SLEEP_DEBUG: False,
    CONF_BLE_SLEEP_CLOCK: "xtal",
    CONF_REQUEST_POLL_WINDOW: "10s",
}
SLEEPY_ONLY = (*SLEEPY_DEFAULTS, CONF_NIGHT_POLL_INTERVAL, CONF_NIGHT_LOOP_INTERVAL)

zigbee_power_ns = cg.esphome_ns.namespace("zigbee_power")
ZigbeePower = zigbee_power_ns.class_("ZigbeePower", cg.Component)
PressTrigger = zigbee_power_ns.class_("PressTrigger", automation.Trigger.template())

# The stack counts the parent's lifetime down by poll_interval / 1000 whole
# seconds per failed poll; below 1 s it never runs out and a node that lost its
# parent stays blind. The parent keeps a message for a sleepy child ~7.5 s.
POLL_INTERVAL = cv.All(
    cv.positive_time_period_milliseconds,
    cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=7)),
)
# The main loop waits up to this long without feeding the 5 s task watchdog
# (ESPHome feeds it at most once a second); while something keeps the chip
# awake that wait counts in real time, so stay well below.
LOOP_INTERVAL = cv.All(
    cv.positive_time_period_milliseconds,
    cv.Range(min=cv.TimePeriod(milliseconds=16), max=cv.TimePeriod(seconds=3)),
)


def _sleepy_options(config):
    if not config[CONF_SLEEPY]:
        unused = [key for key in SLEEPY_ONLY if key in config]
        if unused:
            _LOGGER.warning("zigbee_power: %s only act with sleepy: true", ", ".join(unused))
        return config
    for key, default in SLEEPY_DEFAULTS.items():
        if key not in config:
            config[key] = CONFIG_FIELDS[key](default)
    config.setdefault(CONF_LOOP_INTERVAL, LOOP_INTERVAL("1s"))
    return config


def _timer_guard_option(config):
    c6 = get_esp32_variant() == VARIANT_ESP32C6
    if config.setdefault(CONF_TIMER_GUARD, c6) and not c6:
        raise cv.Invalid("timer_guard is only for the ESP32-C6", [CONF_TIMER_GUARD])
    return config


CONFIG_FIELDS = {
    CONF_POLL_INTERVAL: POLL_INTERVAL,
    CONF_AWAKE_AFTER_BOOT: cv.positive_time_period_milliseconds,
    # Power the digital peripherals down in light sleep (about 180 down to
    # 35 uA for the chip, datasheet). ESP-IDF only does it while every
    # peripheral in use can restore its state; otherwise it stays on.
    CONF_POWER_DOWN_PERIPHERALS: cv.boolean,
    # Counts the light sleeps that powered the peripherals down
    # (sleeps_pd_top()), to check the option above without a meter.
    CONF_SLEEP_DEBUG: cv.boolean,
    # The BLE controller's clock in light sleep. "xtal" (ESP-IDF's default
    # without a 32 kHz crystal) keeps the 40 MHz crystal running, and on
    # the C6 that also keeps the peripherals powered. "rc" uses the
    # internal 136 kHz RC: both can sleep, but Espressif warns that a
    # connection may not hold with the less exact clock.
    CONF_BLE_SLEEP_CLOCK: cv.one_of("xtal", "rc", lower=True),
    # After a request from the network (an interview, a reconfigure, a read
    # or a write) poll every 50 ms until none came for this long: each of the
    # next requests waits at the parent until the next poll. 0s turns it off.
    CONF_REQUEST_POLL_WINDOW: cv.All(
        cv.positive_time_period_milliseconds,
        cv.Range(max=cv.TimePeriod(seconds=60)),
    ),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZigbeePower),
            cv.Optional(CONF_SLEEPY, default=False): cv.boolean,
            # ESPHome's main loop interval (16 ms by default); every round
            # wakes the chip, scheduled work still runs on time. 1 s by
            # default with sleepy: true, ESPHome's own otherwise.
            cv.Optional(CONF_LOOP_INTERVAL): LOOP_INTERVAL,
            # At night (set by hiflow_ble's night mode): poll and main loop
            # less often. Both default to their day values.
            cv.Optional(CONF_NIGHT_POLL_INTERVAL): POLL_INTERVAL,
            cv.Optional(CONF_NIGHT_LOOP_INTERVAL): LOOP_INTERVAL,
            **{cv.Optional(key): validator for key, validator in CONFIG_FIELDS.items()},
            # A button that wakes the chip from light sleep. Its presses are
            # counted in an interrupt, so a short one is not lost while the
            # main loop waits its loop_interval; on_press runs once for each
            # press, also for several within one loop interval. Also
            # works without sleepy.
            cv.Optional(CONF_WAKE_BUTTON): cv.Schema(
                {
                    cv.Required(CONF_PIN): pins.internal_gpio_input_pin_schema,
                    cv.Optional(CONF_ON_PRESS): automation.validate_automation(
                        {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PressTrigger)}
                    ),
                }
            ),
            # GPIOs that keep their normal setting in light sleep instead of
            # floating: a display's control lines, a backlight, an LED's data
            # line.
            cv.Optional(CONF_KEEP_PINS): cv.ensure_list(cv.int_range(min=0, max=30)),
            # Repairs the ESP32-C6's esp_timer when it jumps back (a hardware
            # fault, ESP-IDF issue 19036; see timer_guard.h). On by default
            # on the C6, whatever its revision: which ones have the fault is
            # not known.
            cv.Optional(CONF_TIMER_GUARD): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    only_on_variant(supported=[VARIANT_ESP32C6, VARIANT_ESP32H2], msg_prefix="zigbee_power"),
    _sleepy_options,
    _timer_guard_option,
)


def _final_validate(config):
    full = fv.full_config.get()
    if config[CONF_SLEEPY] and full.get("zigbee", {}).get("router"):
        raise cv.Invalid("zigbee_power sleepy: true and zigbee router: true are mutually exclusive")
    night = [key for key in (CONF_NIGHT_POLL_INTERVAL, CONF_NIGHT_LOOP_INTERVAL) if key in config]
    hiflow = full.get("hiflow_ble", {})
    if night and not ("night_scan_interval" in hiflow or "night_ble_off" in hiflow):
        _LOGGER.warning(
            "zigbee_power: %s only act with hiflow_ble's night mode "
            "(night_scan_interval or night_ble_off)",
            ", ".join(night),
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # zigbee_ota tells this component about a firmware update.
    cg.add_define("USE_ZIGBEE_POWER")
    if CONF_LOOP_INTERVAL in config:
        cg.add(var.set_loop_interval(config[CONF_LOOP_INTERVAL]))
    if button := config.get(CONF_WAKE_BUTTON):
        pin = await cg.gpio_pin_expression(button[CONF_PIN])
        cg.add(var.set_wake_button(pin))
        for conf in button.get(CONF_ON_PRESS, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
    for pin in config.get(CONF_KEEP_PINS, []):
        cg.add(var.add_keep_pin(pin))
    if config[CONF_TIMER_GUARD]:
        # The ESP32-C6's esp_timer can jump back (a hardware fault, ESP-IDF
        # issue 19036); timer_guard repairs it on every reading, see timer_guard.h.
        cg.add_define("USE_ZIGBEE_POWER_TIMER_GUARD")
        cg.add_build_flag("-Wl,--wrap=esp_timer_get_time")
        cg.add_build_flag("-Wl,--wrap=esp_timer_impl_get_time")
        # A repair can run in an interrupt while the flash cache is off: keeps
        # esp_timer_private_advance() in IRAM (also esp_timer's own code).
        add_idf_sdkconfig_option("CONFIG_PM_SLP_IRAM_OPT", True)
    if config[CONF_SLEEPY]:
        cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
        cg.add(var.set_awake_after_boot(config[CONF_AWAKE_AFTER_BOOT]))
        cg.add(var.set_night_poll_interval(config.get(CONF_NIGHT_POLL_INTERVAL, config[CONF_POLL_INTERVAL])))
        cg.add(var.set_night_loop_interval(config.get(CONF_NIGHT_LOOP_INTERVAL, config[CONF_LOOP_INTERVAL])))
        cg.add(var.set_request_poll_window(config[CONF_REQUEST_POLL_WINDOW]))
        cg.add_define("USE_ZIGBEE_POWER_SLEEPY")
        # The 802.15.4 interrupt of a sleepy node switches the PHY off, which
        # aborts while a task holds the PHY's mutex; see phy_guard.h.
        cg.add_build_flag("-Wl,--wrap=esp_phy_enable")
        cg.add_build_flag("-Wl,--wrap=esp_phy_disable")
        # phy_guard.cpp includes esp_phy's headers. ESPHome leaves esp_phy out by default
        # and ieee802154 links it privately; only BLE (bt -> esp_wifi -> esp_phy) puts the
        # headers on the include path, so a build without BLE needs this.
        include_builtin_idf_component("esp_phy")
        # The set of Espressif's light_sleep_end_device example, without the
        # flash power-down. In every light sleep ESP-IDF switches all pads to
        # their sleep setting (isolated: no output, no pull), with or without
        # the peripheral power-down: GPIO outputs float while the chip sleeps.
        # The XIAO's RF switch does not mind (the radio sleeps too); a pin
        # that must hold its level (a backlight, a display's reset line)
        # goes into keep_pins.
        add_idf_sdkconfig_option("CONFIG_PM_ENABLE", True)
        add_idf_sdkconfig_option("CONFIG_FREERTOS_USE_TICKLESS_IDLE", True)
        add_idf_sdkconfig_option("CONFIG_IEEE802154_SLEEP_ENABLE", True)
        add_idf_sdkconfig_option("CONFIG_ESP_PHY_MAC_BB_PD", True)
        add_idf_sdkconfig_option("CONFIG_BT_LE_SLEEP_ENABLE", True)
        add_idf_sdkconfig_option("CONFIG_PM_LIGHT_SLEEP_CALLBACKS", True)
        if config[CONF_POWER_DOWN_PERIPHERALS]:
            add_idf_sdkconfig_option("CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP", True)
        if config[CONF_BLE_SLEEP_CLOCK] == "rc":
            add_idf_sdkconfig_option("CONFIG_BT_LE_LP_CLK_SRC_MAIN_XTAL", False)
            add_idf_sdkconfig_option("CONFIG_BT_LE_LP_CLK_SRC_DEFAULT", True)
        if config[CONF_SLEEP_DEBUG]:
            add_idf_sdkconfig_option("CONFIG_ESP_SLEEP_DEBUG", True)
            cg.add_define("USE_ZIGBEE_POWER_SLEEP_DEBUG")
