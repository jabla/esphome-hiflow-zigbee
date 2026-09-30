"""ZHA quirk for the HiFlow Pro Zigbee bridge (esphome-hiflow-zigbee): the range
of the power limit slider.

ZHA takes the minimum, maximum and step of an Analog Output from its
attribute cache only and never reads them from the device. A re-interview
starts with an empty cache: after every update over Zigbee, after
*Reconfigure*, after a re-join. The slider then falls back to 0-1023 in steps
of 1. The bridge's slider is fixed at 0-100 % in 10 % steps, so this quirk
answers with those values while the cache is empty; a value in the cache
still wins.

Install: copy this file into the folder that `zha: custom_quirks_path:` in
configuration.yaml points to, and restart Home Assistant.
"""

from zhaquirks.builder import QuirkBuilder
from zigpy.quirks import CustomCluster
from zigpy.zcl.clusters.general import AnalogOutput

POWER_LIMIT_ENDPOINT = 32

RANGE = {
    AnalogOutput.AttributeDefs.min_present_value.id: 0.0,
    AnalogOutput.AttributeDefs.max_present_value.id: 100.0,
    AnalogOutput.AttributeDefs.resolution.id: 10.0,
}


class HiflowPowerLimit(CustomCluster, AnalogOutput):
    """The power limit slider: its range, even with an empty attribute cache."""

    def get(self, key, default=None):
        result = super().get(key)
        if result is not None:
            return result
        try:
            attr_def = self.find_attribute(key)
        except KeyError:
            return default
        return RANGE.get(attr_def.id, default)


(
    QuirkBuilder("esphome", "HMS-2000-4WB Bridge")
    .replaces(HiflowPowerLimit, endpoint_id=POWER_LIMIT_ENDPOINT)
    .add_to_registry()
)
