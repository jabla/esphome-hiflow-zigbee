#!/usr/bin/env python3
"""Wraps an ESPHome build into a Zigbee OTA file (ZCL spec 11.4) for the
zigbee_ota component.

File version, manufacturer code and image type come from zigbee_ota.json,
which the zigbee_ota component writes into the build directory at every
compile: the same numbers the firmware reports.

The image goes in as one sub-element:
  delta  with --from and the running image kept here: an esp_delta_ota patch
         (tag 0xF101) against it, a few percent of the image for a small
         change. Only that image accepts it (SHA-256 in the patch header).
         Needs `detools` (pip install detools).
  zlib   otherwise (also without detools): the compressed image (tag
         0xF100), about 60 %.
  heatshrink  instead of zlib for an ESP32-H2: its heap has no room for
         zlib's 32 KB window. The image as a detools patch from nothing (tag
         0xF102), about 85 %. Needs `detools`.

Every image this tool packs, and every image tools/flash_config.sh flashes, is
kept as .esphome/zigbee_ota/<name>-<VERSION>.bin next to the config, and the
OTA file goes next to it. `--from` takes the version the device runs, as the
update entity in Home Assistant shows it (installed version), or a file.

With --from the tool also checks that the device will take the file: the
coordinator only offers a version above the installed one. For a device that
still runs an image from before the date versions (Unix time, image type
0x4846 on every board) it writes a second file, <name>-<VERSION>-from-<OLD>.ota,
whose header claims the old image type and the installed version + 1; the
firmware in it reports its own version and the board's image type from then
on, and finds the first file as its installed version.

Next to the OTA files the tool keeps index.json, an index for zigpy's
`zigpy_local` provider with a short note per image (date, build, label, git
version) and the --notes text. It is optional: the image works without it.

Usage: python3 tools/make_zigbee_ota.py <build dir> [--from 0x26100301 | --from old.bin] [--notes TEXT]
  <build dir> is .esphome/build/<name>, next to the config.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import re
import struct
import sys
import zlib
from pathlib import Path

# The version scheme lives with the component, which the tool shares.
_spec = importlib.util.spec_from_file_location(
    "zigbee_ota_version", Path(__file__).resolve().parent.parent / "components" / "zigbee_ota" / "version.py")
versions = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(versions)

FILE_IDENTIFIER = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56          # no optional fields
STACK_VERSION_PRO = 0x0002
TAG_IMAGE_ZLIB = 0xF100
TAG_IMAGE_DELTA = 0xF101
TAG_IMAGE_HEATSHRINK = 0xF102
ESP_IMAGE_MAGIC = 0xE9
DELTA_MAGIC = 0xFCCDDE10
DELTA_HEADER_LEN = 64
# Chip ids (offset 12 of the image header) that take a full image as
# heatshrink instead of zlib: too little heap for the inflater.
CHIP_ID_ESP32H2 = 16
HEATSHRINK_CHIPS = {CHIP_ID_ESP32H2}
# Image type of every image from before the date versions, on every board.
LEGACY_IMAGE_TYPE = 0x4846
INDEX_NAME = "index.json"
# Above the folder provider's copy of the same file, so the coordinator shows
# the note from the index.
INDEX_SPECIFICITY = 1
# Home Assistant's limit for the release summary.
SUMMARY_MAX = 255


def app_digest(image: bytes) -> bytes:
    """The SHA-256 ESP-IDF appends to an app image, which is what
    esp_partition_get_sha256() returns for the partition holding it."""
    if image[0] != ESP_IMAGE_MAGIC or image[23] != 1:
        raise SystemExit("not an ESP app image with an appended SHA-256")
    return image[-32:]


def detools_patch(base: bytes, new: bytes) -> bytes:
    """A detools patch in the one compression esp_delta_ota decodes, checked
    before it goes anywhere near a device."""
    try:
        import detools
    except ImportError:
        raise SystemExit("this image needs detools: pip install detools")
    patch = io.BytesIO()
    detools.create_patch(io.BytesIO(base), io.BytesIO(new), patch, compression="heatshrink")
    body = patch.getvalue()
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(body), out)
    if out.getvalue() != new:
        raise SystemExit("patch does not reproduce the new image")
    return body


def chip_id(image: bytes) -> int:
    return struct.unpack_from("<H", image, 12)[0]


def delta_patch(base: bytes, new: bytes) -> bytes:
    body = detools_patch(base, new)
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


def parse_base(base: str, keep: Path, name: str) -> tuple[int | None, Path | None]:
    """The installed version and the kept image of it, from --from (a version
    or a file). A file's version comes from its name, <name>-<VERSION>.bin."""
    if Path(base).is_file():
        m = re.search(r"-([0-9A-Fa-f]{8})\.bin$", base)
        return (int(m.group(1), 16) if m else None), Path(base)
    try:
        version = int(base, 16)
    except ValueError:
        raise SystemExit(f"--from {base}: neither a file nor a version like 0x26100301")
    kept = keep / f"{name}-{version:08X}.bin"
    return version, (kept if kept.is_file() else None)


def header_for(version: int, image_type: int, installed: int | None) -> tuple[int, int, bool]:
    """Version and image type for the file header, and whether this file moves
    a device off the old Unix-time versions."""
    if installed is None:
        return version, image_type, False
    if not versions.is_date_version(installed):
        return installed + 1, LEGACY_IMAGE_TYPE, True
    if version <= installed:
        raise SystemExit(
            f"version 0x{version:08X} is not above the installed 0x{installed:08X}: the coordinator "
            "would not offer it. Was the device updated from another build directory? Compile again.")
    return version, image_type, False


def summary(info: dict, version: int, transition: bool) -> str:
    parts = [versions.describe(version), info.get("label")]
    if info.get("git"):
        parts.append(info["git"])
    text = ", ".join(p for p in parts if p)
    if transition:
        text = "Moves to date versions: " + text
    return text[:SUMMARY_MAX]


def update_index(keep: Path, ota: Path, data: bytes, manufacturer: int, image_type: int,
                 version: int, changelog: str, notes: str | None) -> Path:
    """Puts the file into index.json, in place of the last one for the same
    manufacturer code and image type. A broken index is started anew."""
    index_file = keep / INDEX_NAME
    try:
        index = json.loads(index_file.read_text())
        firmwares = [fw for fw in index["firmwares"]
                     if (fw["manufacturer_id"], fw["image_type"]) != (manufacturer, image_type)]
    except (OSError, ValueError, KeyError, TypeError):
        firmwares = []
    entry = {
        "path": ota.name,
        "file_version": version,
        "file_size": len(data),
        "image_type": image_type,
        "manufacturer_id": manufacturer,
        "checksum": "sha3-256:" + hashlib.sha3_256(data).hexdigest(),
        "changelog": changelog,
        "specificity": INDEX_SPECIFICITY,
    }
    if notes:
        entry["release_notes"] = notes
    firmwares.append(entry)
    firmwares.sort(key=lambda fw: (fw["manufacturer_id"], fw["image_type"]))
    index_file.write_text(json.dumps({"firmwares": firmwares}, indent=2) + "\n")
    return index_file


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--from", dest="base", help="installed version (0x...) or image file running on the device")
    ap.add_argument("--notes", help="release notes for the update dialog (Markdown), optional")
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

    installed, base_file = (None, None) if args.base is None else parse_base(args.base, keep, name)
    head_version, head_type, transition = header_for(version, image_type, installed)
    (keep / f"{name}-{version:08X}.bin").write_bytes(image)

    heatshrink = chip_id(image) in HEATSHRINK_CHIPS
    if base_file is not None and importlib.util.find_spec("detools") is None:
        if heatshrink:
            print("this chip needs detools for every image: pip install detools", file=sys.stderr)
            return 1
        print("detools is not installed (pip install detools), so a full image instead of a delta")
        base_file = None
    elif base_file is None and args.base is not None:
        print(f"no kept image of 0x{installed:08X} here, so a full image instead of a delta")
    if base_file is not None:
        base = base_file.read_bytes()
        # The chip id in the image header (offset 12): a C6 image is no base
        # for an H2 build. The device would refuse the patch anyway.
        if base[12:14] != image[12:14]:
            print(f"{base_file.name} was built for another chip", file=sys.stderr)
            return 1
        fmt, tag, payload = "delta", TAG_IMAGE_DELTA, delta_patch(base, image)
    elif heatshrink:
        fmt, tag, payload = "heatshrink", TAG_IMAGE_HEATSHRINK, detools_patch(b"", image)
    else:
        fmt, tag, payload = "zlib", TAG_IMAGE_ZLIB, zlib.compress(image, 9)

    out = args.output or keep / f"{name}-{version:08X}.ota"
    label = info.get("label") or name
    text = f"{version:08X} {label}"
    data = build(tag, payload, manufacturer, image_type, version, text)
    out.write_bytes(data)
    print(f"{out}: version 0x{version:08X} ({versions.describe(version)}), image {len(image)} bytes, "
          f"{fmt} {len(payload)} bytes ({len(payload) * 100 // len(image)} %), file {len(data)} bytes")
    index = update_index(out.parent, out, data, manufacturer, image_type, version,
                         summary(info, version, False), args.notes)

    if transition:
        # The same image under the old header, the one the old firmware takes. The
        # file above, with the board's image type, is what the coordinator then
        # finds for the updated device: the update entity shows it as up to date
        # instead of having no latest version.
        moving = out.with_name(f"{out.stem}-from-{installed:08X}.ota")
        moving_data = build(tag, payload, manufacturer, head_type, head_version, text)
        moving.write_bytes(moving_data)
        update_index(out.parent, moving, moving_data, manufacturer, head_type, head_version,
                     summary(info, version, True), args.notes)
        print(f"{moving}: the device runs 0x{installed:08X} from before the date versions; this file's "
              f"header says 0x{head_version:08X} and image type 0x{head_type:04X}, so the coordinator "
              f"offers it. Afterwards the device reports 0x{version:08X} and image type "
              f"0x{image_type:04X}. Copy both files.")

    print(f"{index}: updated (optional, for the zigpy_local provider)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
