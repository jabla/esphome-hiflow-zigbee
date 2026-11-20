# Own build

## What you need

- ESPHome **2026.11.0** or newer
- Python 3 for the helper scripts

## Quick start

1. **Settings.** Copy `hiflow_secrets.example.yaml` to `hiflow_secrets.yaml` and set `board`.
   The inverter values may stay empty: the bridge then finds the inverter and takes the PIN
   over USB, like a release image. Values set here win over USB. The comments in the file
   explain each value.
2. **Time zone.** Set `offset` and `eu_dst` under `hiflow_ble:` in `hiflow-zb.yaml`. The
   default is CET with European summer time.
3. **Build** before you open pairing. The first build takes several minutes.
   ```bash
   esphome compile hiflow-zb.yaml
   ```
4. **Open pairing, then flash** over USB:
   ```bash
   DEV=/dev/ttyACM0 tools/flash_config.sh hiflow-zb.yaml
   ```
   `DEV` is the board's port: *Espressif USB JTAG* in `ls /dev/serial/by-id/`, not the Zigbee
   stick. The script erases the flash if the board ran other firmware. Reflashing the bridge
   keeps its settings and its pairing. Use `esphome run` only after `esptool erase_flash`.

   The board joins as `HMS-2000-4WB Bridge`. Missed the pairing window: open pairing again and
   reset the board.

Then put the board at the inverter on a USB charger.

## Options

- **Receiver always on:** `sleepy: "false"` in `hiflow_secrets.yaml`. Switching the mode makes
  the bridge leave the network and join again: open pairing before you flash.
- **C6-LCD display:** brightness, page time and night mode (`glow`, `off`, `screensaver`) are
  substitutions at the top of `boards/waveshare_c6_lcd147.yaml`.

## Firmware updates over Zigbee

1. **Once, in ZHA:** a folder for update files, in `configuration.yaml`:
   ```yaml
   zha:
     zigpy_config:
       ota:
         extra_providers:
           - type: advanced
             path: /config/zigpy_ota
             warning: "I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk."
   ```
   Create the folder and restart Home Assistant. The bridge gets an update entity that shows
   the installed version.
2. **Build and pack:**
   ```bash
   esphome compile hiflow-zb.yaml
   python3 -m venv .venv && .venv/bin/pip install detools
   .venv/bin/python tools/make_zigbee_ota.py .esphome/build/hiflow-zb --from 0x27031401
   ```
   `--from` is the installed version. With it the file holds a small delta, without it the full
   image.
3. **Install:** copy the `.ota` file from `.esphome/zigbee_ota/` into `/config/zigpy_ota`,
   restart Home Assistant and press *Install* on the update entity.

The version is the build date plus the build of that day, in hex: `0x27031402` is the second
build on 14 March 2027.

A board on a weak supply, such as a XIAO on a laptop's USB port, can brown out during a
download. Then set `tx_power: 0` under `zigbee_ota:`, see
[`components/zigbee_ota/README.md`](../components/zigbee_ota/README.md).

### Update notes (optional)

The tool also writes `index.json` for zigpy's `zigpy_local` provider. With it the update dialog
shows the build and the text of `--notes "..."`. Copy it into `/config/zigpy_ota` **first**,
then add the provider below the folder:
```yaml
        - type: zigpy_local
          index_file: /config/zigpy_ota/index.json
```
Copy `index.json` along with every `.ota` file. **Never delete it or the folder while the
provider is in `configuration.yaml`**: ZHA does not start without them.

### In Zigbee2MQTT

Zigbee2MQTT shows no update entity for the bridge. Start the update over MQTT with a local file:

1. Build and pack as in step 2 above. `--from` is the version `tools/flash_config.sh` printed
   (`kept as ... version 0x...`) or the version of the last update.
2. Copy the `.ota` file into Zigbee2MQTT's `data/ota/` folder.
3. Publish to `zigbee2mqtt/bridge/request/device/ota_update/update`, with the path as
   Zigbee2MQTT sees it:
   ```json
   {"id": "<device>", "url": "/app/data/ota/<file>.ota",
    "image_block_response_delay": 20, "default_maximum_data_size": 64}
   ```
   The two block settings make the download about twice as fast.

## Switching to the release images

Add the release index as in [*From the releases*](../README.md#from-the-releases). The next
release then comes as an OTA update. The bridge keeps the settings of your own build.
