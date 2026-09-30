# zigbee_ota

Firmware updates over Zigbee for ESPHome's `zigbee` component on the ESP32-C6: the ZCL OTA
Upgrade client of esp-zigbee-lib, which ESPHome does not expose. How to use it with ZHA is in
the main README, *Firmware updates over Zigbee*.

```yaml
zigbee_ota:
  # all optional
  endpoint: 1                # endpoint for the OTA Upgrade client cluster
  manufacturer_code: 0x131B  # Espressif
  image_type: 0x4846
  verify_timeout: 10min
  tx_power: 20               # dBm; unset keeps the stack default
```

- `endpoint`: any endpoint that the zigbee codegen creates; 1 exists as soon as there is one
  Zigbee sensor. Adding the cluster to a bridge that is already paired needs a re-interview
  (ZHA: *Reconfigure*, then reload the integration).
- `manufacturer_code`, `image_type`: must match the OTA file; `tools/make_zigbee_ota.py` takes
  them, and the file version, from `zigbee_ota.json` in the build directory.
- `verify_timeout`: a new image counts as good once the OTA server answered it. If it cannot
  reach the server within this time, it rolls back to the previous image.
- `tx_power`: only for a board whose supply browns out while it downloads (flash writes on
  top of full-power sending).

What it does:

- The file version is the Unix time of the code generation, newer with every compile.
- Images come zlib-compressed (tag `0xF100`), or as an
  [esp_delta_ota](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota)
  patch against the running image (tag `0xF101`). A patch for any other image is refused
  before anything is written (SHA-256 in its header). `esp_ota_end()` checks the result.
- The bootloader's app rollback is turned on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), and
  ESPHome no longer marks the image valid at boot; this component does, after the OTA server
  answered a Query Next Image. An image that crashes before that is replaced by the previous
  one at the next boot. The bootloader only changes with a USB flash.
- esp-zigbee-lib 2.0.4 crashes when the client finishes an upgrade the regular way: it sends a
  packet that was never set up and fails an assert, before it acknowledges the server's
  Upgrade End Response. ZHA then reports the update as failed although it worked. The
  component answers the apply step with an abort instead, which takes the library's sound
  path, and reboots into the new image 2 seconds later.
