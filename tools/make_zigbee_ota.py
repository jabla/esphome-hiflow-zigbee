#!/usr/bin/env python3
"""Wraps an ESPHome build into a Zigbee OTA file (ZCL spec 11.4) for the
zigbee_ota component.

File version, manufacturer code and image type come from zigbee_ota.json,
which the zigbee_ota component writes into the build directory at every
compile: the same numbers the firmware reports.

The image goes in as one sub-element:
  delta  with --from: an esp_delta_ota patch (tag 0xF101) against the image
         running on the device, a few percent of the image for a small
         change. Only that image accepts it (SHA-256 in the patch header).
         Needs `detools` (pip install detools).
  zlib   without a base: the compressed image (tag 0xF100), about 60 %.

Every image this tool packs, and every image tools/flash_config.sh flashes, is
kept as .esphome/zigbee_ota/<name>-<VERSION>.bin next to the config, and the
OTA file goes next to it. `--from` takes the version the device runs, as the
update entity in Home Assistant shows it (installed version), or a file.

Usage: python3 tools/make_zigbee_ota.py <build dir> [--from 0x6ABBD143 | --from old.bin]
  <build dir> is .esphome/build/<name>, next to the config.
"""
import argparse
import io
import json
import struct
import sys
import zlib
from pathlib import Path

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56          # no optional fields
STACK_VERSION_PRO = 0x0002
TAG_IMAGE_ZLIB = 0xF100
TAG_IMAGE_DELTA = 0xF101
ESP_IMAGE_MAGIC = 0xE9
DELTA_MAGIC = 0xFCCDDE10
DELTA_HEADER_LEN = 64


def app_digest(image: bytes) -> bytes:
    """The SHA-256 ESP-IDF appends to an app image, which is what
    esp_partition_get_sha256() returns for the partition holding it."""
    if image[0] != ESP_IMAGE_MAGIC or image[23] != 1:
        raise SystemExit("not an ESP app image with an appended SHA-256")
    return image[-32:]


def delta_patch(base: bytes, new: bytes) -> bytes:
    try:
        import detools
    except ImportError:
        raise SystemExit("--from needs detools: pip install detools")
    patch = io.BytesIO()
    detools.create_patch(io.BytesIO(base), io.BytesIO(new), patch, compression="heatshrink")
    body = patch.getvalue()
    # Check the patch before it goes anywhere near a device.
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(body), out)
    if out.getvalue() != new:
        raise SystemExit("delta patch does not reproduce the new image")
    header = struct.pack("<I", DELTA_MAGIC) + app_digest(base)
    return header.ljust(DELTA_HEADER_LEN, b"\0") + body


def build(tag: int, payload: bytes, manufacturer: int, image_type: int, version: int, text: str) -> bytes:
    element = struct.pack("<HI", tag, len(payload)) + payload
    total = HEADER_LENGTH + len(element)
    header = struct.pack(
        "<IHHHHHIH32sI",
        FILE_IDENTIFIER,
        HEADER_VERSION,
        HEADER_LENGTH,
        0,                          # field control: no optional fields
        manufacturer,
        image_type,
        version,
        STACK_VERSION_PRO,
        text.encode()[:32].ljust(32, b"\0"),
        total,
    )
    assert len(header) == HEADER_LENGTH
    return header + element


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--from", dest="base", help="version (0x...) or image file running on the device")
    args = ap.parse_args()

    info_file = args.build_dir / "zigbee_ota.json"
    if not info_file.is_file():
        print(f"{info_file} missing: is zigbee_ota in the config?", file=sys.stderr)
        return 1
    info = json.loads(info_file.read_text())
    version = info["file_version"]
    manufacturer = info["manufacturer_code"]
    image_type = info["image_type"]
    firmware = args.build_dir / "build" / "firmware.ota.bin"
    image = firmware.read_bytes()
    if image[0] != ESP_IMAGE_MAGIC:
        print(f"{firmware} is not an ESP app image", file=sys.stderr)
        return 1
    # The code generation writes zigbee_ota.json before the compile; an image
    # older than it belongs to an earlier (or failed) build and would claim
    # the wrong version.
    if firmware.stat().st_mtime < info_file.stat().st_mtime:
        print(f"{firmware} is older than {info_file.name}: the last compile did not finish", file=sys.stderr)
        return 1

    app_digest(image)
    name = args.build_dir.name
    keep = args.build_dir.parent.parent / "zigbee_ota"
    keep.mkdir(exist_ok=True)
    (keep / f"{name}-{version:08X}.bin").write_bytes(image)

    if args.base is not None:
        if Path(args.base).is_file():
            base_file = Path(args.base)
        else:
            base_file = keep / f"{name}-{int(args.base, 16):08X}.bin"
            if not base_file.is_file():
                print(f"no kept image {base_file.name}; kept: "
                      f"{', '.join(sorted(p.stem for p in keep.glob(name + '-*.bin')))}", file=sys.stderr)
                return 1
        fmt, tag, payload = "delta", TAG_IMAGE_DELTA, delta_patch(base_file.read_bytes(), image)
    else:
        fmt, tag, payload = "zlib", TAG_IMAGE_ZLIB, zlib.compress(image, 9)

    out = args.output or keep / f"{name}-{version:08X}.ota"
    data = build(tag, payload, manufacturer, image_type, version, f"{name} {version:08X}")
    out.write_bytes(data)
    print(f"{out}: version 0x{version:08X}, image {len(image)} bytes, "
          f"{fmt} {len(payload)} bytes ({len(payload) * 100 // len(image)} %), file {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
