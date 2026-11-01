# zigbee_ota

Firmware updates over Zigbee for ESPHome's `zigbee` component on the ESP32-C6 and ESP32-H2: the ZCL OTA
Upgrade client of esp-zigbee-lib, which ESPHome does not expose. How to use it with ZHA is in
the main README, *Firmware updates over Zigbee*.

```yaml
zigbee_ota:
  # all optional
  endpoint: 1                # endpoint for the OTA Upgrade client cluster
  manufacturer_code: 0x131B  # Espressif
  image_type: 0x4846         # one per board
  label: my-board            # names the build in the OTA file and the update notes
  verify_timeout: 10min
  tx_power: 20               # dBm; unset keeps the stack default
```

- `endpoint`: any endpoint that the zigbee codegen creates; 1 exists as soon as there is one
  Zigbee sensor. Adding the cluster to a bridge that is already paired needs a re-interview
  (ZHA: *Reconfigure*, then reload the integration).
- `manufacturer_code`, `image_type`: must match the OTA file; `tools/make_zigbee_ota.py` takes
  them, and the file version, from `zigbee_ota.json` in the build directory. The coordinator
  offers an image to every device with the same pair, so give each board its own image type.
- `label`: defaults to the node name.
- `verify_timeout`: a new image counts as good once the OTA server answered it. If it cannot
  reach the server within this time, it rolls back to the previous image.
- `tx_power`: only for a board whose supply browns out while it downloads (flash writes on
  top of full-power sending).

What it does:

- The file version is the date of the code generation and the build of that day,
  `0xYYMMDDNN` (`version.py`), newer with every compile. The count goes on from the last
  compile of every config in the directory and the images kept under `.esphome/zigbee_ota/`,
  so a second config for the same board (a debug copy) never ends up below the first; more
  than 99 builds a day stop the compile.
- Images come zlib-compressed (tag `0xF100`), or as an
  [esp_delta_ota](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota)
  patch against the running image (tag `0xF101`). A patch for any other image is refused
  before anything is written (SHA-256 in its header). `esp_ota_end()` checks the result.
- On the ESP32-H2 a full image comes as a patch from nothing instead (tag `0xF102`,
  heatshrink, about 85 % of the image): zlib's inflater needs a 32 KB window plus 11 KB of
  state, more than the largest free block there. The patch engine needs a few hundred bytes.
- The bootloader's app rollback is turned on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), and
  ESPHome no longer marks the image valid at boot; this component does, after the OTA server
  answered a Query Next Image. An image that crashes before that is replaced by the previous
  one at the next boot. The bootloader only changes with a USB flash.
- It needs esp-zigbee-lib 2.0.5 or newer. ESPHome 2026.10 asks for 2.0.4, so the component
  raises that request (the build log notes the version change) and refuses to build against
  an older library. 2.0.4 crashes when the client finishes an upgrade: it sends a packet that
  was never set up and fails an assert. 2.0.5 fixed that. It also starts in about a second
  where 2.0.4 took 30 s to 2 minutes: 2.0.4 reads every stored reporting entry once per
  endpoint (each read a walk through the whole NVS) and writes them all again at every boot.
- After the upgrade finished the component reboots into the new image, 2 seconds later or
  after the delay the server asked for. An upgrade that stops between the selection of the
  image and the finish puts the running image back as the boot image.
- An offer that arrives before the new image was marked valid counts as the server's answer:
  the image is marked valid and the download starts (the first update after an update used to
  fail once).
- A block answer that does not continue the data is skipped: a late one that arrives after
  the answer to the retried request, or one from beyond the data because Zigbee2MQTT dropped a
  request (it takes one at a time). The library does not check the offset of an answer, so
  the component keeps track of the requests and moves the library's file offset back when the
  request for the missing block got lost (`ota_blocks.h`, host test
  `test/host/test_ota_blocks.c`). More than 32 answers in a row that the requests still in
  flight do not explain end the download.
- A download without progress for 60 s is ended: the server answered a block with an error
  (a ZHA reload mid-download does that), and the library stops without telling. The
  component frees the download and resets the upgrade status, so the next offer is taken.
- With [`zigbee_power`](../zigbee_power/README.md) `sleepy: true` the node polls its parent
  every 50 ms from the start of a download to the reboot (it still sleeps between polls).
