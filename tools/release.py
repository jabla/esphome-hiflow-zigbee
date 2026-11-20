#!/usr/bin/env python3
"""The files a GitHub release carries for updates over Zigbee, and the web
installer built from them.

Release images are built from hiflow_secrets.example.yaml as it is, so no image
holds personal data: the bridge finds its inverter by its BLE name and takes
the rest over USB (see "Settings over USB" in components/hiflow_ble/README.md). Per board a
release carries

  hiflow-zb-<board>-<VERSION>.factory.bin       the first install, by USB
  hiflow-zb-<board>-<VERSION>.app.bin           the app image, base of the next delta
  hiflow-zb-<board>-<VERSION>.ota               the full image, over Zigbee
  hiflow-zb-<board>-<VERSION>-from-<OLD>.ota    a delta for a bridge on the release before

and one index.json in Zigbee2MQTT's format with the URLs of these files. ZHA
reads it with the OTA provider type `z2m`, Zigbee2MQTT with
`zigbee_ota_override_index_location`, both from
https://github.com/<owner>/<repo>/releases/latest/download/index.json.

  release.py version [--date YYYY-MM-DD] [--index old/index.json ...]
      The version for a release built on that day (UTC today by default).
      Build number: starts at 1 each day, incrementing.
  release.py pack BOARD BUILD_DIR --url-base URL --out DIR [--previous DIR] [--notes-url URL]
      One board's build (`esphome compile` with ZIGBEE_OTA_FILE_VERSION set to
      the release version) as release files in DIR, plus DIR/index-BOARD.json.
      --previous holds the previous release's app image of the board
      (hiflow-zb-BOARD-<OLD>.app.bin) for the delta.
  release.py index --out FILE PART.json...
      One index from the boards' parts, the deltas first: Zigbee2MQTT takes the
      first entry that fits, ZHA prefers the more specific one anyway.
  release.py site --out DIR --tag TAG --repo-url URL --esp-web-tools DIR --esptool-js DIR FACTORY.bin...
      The web installer for GitHub Pages in DIR: web/index.html, the board
      pictures, the release's factory images, one manifest per board, ESP Web
      Tools' flash() and esptool-js (both from the folders of their npm
      packages, with their licenses).

Usage in .github/workflows/release.yml and pages.yml; pack needs detools for
the deltas and for the ESP32-H2's images.
"""
import argparse
import datetime
import hashlib
import html
import importlib.util
import json
import posixpath
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("zigbee_ota_version", ROOT / "components" / "zigbee_ota" / "version.py")
versions = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(versions)

PACKER = ROOT / "tools" / "make_zigbee_ota.py"
# What the bridge reports in its Basic cluster (hiflow-zb.yaml, ESPHome's default
# manufacturer). zigpy offers an entry only to a device with this model and name.
MODEL_ID = "HMS-2000-4WB Bridge"
MANUFACTURER_NAME = "esphome"
OTA_MAGIC = b"\x1e\xf1\xee\x0b"

# The boards of a release (the matrix in .github/workflows/release.yml): the name
# the web installer shows in its dialog and the chip of the board, in ESP Web
# Tools' names. The installer only tells chips apart, so it needs the name to
# make a wrong pick between the two ESP32-C6 boards visible.
BOARDS = {
    "waveshare_c6_lcd147": ("Waveshare ESP32-C6-LCD-1.47", "ESP32-C6"),
    "xiao_esp32c6": ("Seeed Studio XIAO ESP32-C6", "ESP32-C6"),
    "waveshare_h2_zero": ("Waveshare ESP32-H2-Zero", "ESP32-H2"),
}
# ESP-IDF's esp_chip_id_t, from the header of the bootloader a factory image
# starts with.
CHIP_IDS = {13: "ESP32-C6", 16: "ESP32-H2"}
ESP_IMAGE_MAGIC = 0xE9
WEB = ROOT / "web"


def ota_header(data: bytes) -> dict:
    """The fields of a Zigbee OTA file header (ZCL spec 11.4.2) an index needs."""
    if data[:4] != OTA_MAGIC:
        raise SystemExit("not a Zigbee OTA file")
    _, _, _, manufacturer, image_type, version = struct.unpack_from("<HHHHHI", data, 4)
    return {
        "manufacturerCode": manufacturer,
        "imageType": image_type,
        "fileVersion": version,
        "otaHeaderString": data[20:52].split(b"\0")[0].decode(errors="replace"),
    }


def index_entry(path: Path, url: str, base: int | None, notes_url: str | None) -> dict:
    data = path.read_bytes()
    entry = {
        **ota_header(data),
        "fileName": path.name,
        "fileSize": len(data),
        "sha512": hashlib.sha512(data).hexdigest(),
        "url": url,
        "modelId": MODEL_ID,
        "manufacturerName": [MANUFACTURER_NAME],
    }
    if base is not None:
        entry["minFileVersion"] = entry["maxFileVersion"] = base
    if notes_url:
        entry["releaseNotes"] = notes_url
    return entry


def previous_app(previous: Path | None, board: str) -> tuple[int, Path] | None:
    """The previous release's app image of the board and its version."""
    if previous is None or not previous.is_dir():
        return None
    found = []
    for path in previous.glob(f"hiflow-zb-{board}-*.app.bin"):
        m = re.fullmatch(rf"hiflow-zb-{re.escape(board)}-([0-9A-Fa-f]{{8}})\.app\.bin", path.name)
        if m:
            found.append((int(m.group(1), 16), path))
    return max(found) if found else None


def run_packer(build_dir: Path, out: Path, base: int | None) -> None:
    """tools/make_zigbee_ota.py into a scratch folder (it also writes its own
    index.json next to the file), then only the OTA file into place."""
    with tempfile.TemporaryDirectory() as scratch:
        tmp = Path(scratch) / out.name
        cmd = [sys.executable, str(PACKER), str(build_dir), "-o", str(tmp)]
        if base is not None:
            cmd += ["--from", f"0x{base:08X}"]
        subprocess.run(cmd, check=True)
        shutil.copyfile(tmp, out)


def factory_chip(head: bytes) -> str:
    """The chip a factory image is built for: chip_id (byte 12) in the header of
    the bootloader at its start (esp_image_header_t)."""
    if len(head) < 24 or head[0] != ESP_IMAGE_MAGIC:
        raise SystemExit("not a factory image: no bootloader at its start")
    chip_id = struct.unpack_from("<H", head, 12)[0]
    if chip_id not in CHIP_IDS:
        raise SystemExit(f"a factory image for chip id {chip_id}, not a chip of the boards")
    return CHIP_IDS[chip_id]


def web_manifest(board: str, factory: Path, tag: str) -> dict:
    """ESP Web Tools' manifest for one board: the factory image at offset 0.
    flash() takes the build of the chip it finds, so a board with another chip
    gets nothing written."""
    name, chip = BOARDS[board]
    with factory.open("rb") as f:
        found = factory_chip(f.read(24))
    if found != chip:
        raise SystemExit(f"{factory.name}: an image for the {found}, but the {name} has an {chip}")
    return {
        "name": f"HiFlow Zigbee bridge for the {name}",
        "version": tag,
        "builds": [{"chipFamily": chip, "parts": [{"path": factory.name, "offset": 0}]}],
    }


def cmd_version(args) -> int:
    day = datetime.date.fromisoformat(args.date) if args.date else datetime.datetime.now(datetime.UTC).date()
    known = []
    for index in args.index or []:
        try:
            known += [int(e["fileVersion"]) for e in json.loads(Path(index).read_text())]
        except (OSError, ValueError, KeyError, TypeError) as err:
            raise SystemExit(f"{index}: not a release index ({err})")
    try:
        version = versions.next_version(day, known)
    except ValueError as err:
        raise SystemExit(str(err)) from None
    print(f"0x{version:08X}")
    return 0


def cmd_pack(args) -> int:
    build_dir = args.build_dir
    info = json.loads((build_dir / "zigbee_ota.json").read_text())
    version = int(info["file_version"])
    name = build_dir.name
    stem = f"hiflow-zb-{args.board}-{version:08X}"
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    url_base = args.url_base.rstrip("/")
    entries = []

    for src, suffix in (("firmware.factory.bin", ".factory.bin"), ("firmware.ota.bin", ".app.bin")):
        shutil.copyfile(build_dir / "build" / src, out / f"{stem}{suffix}")

    full = out / f"{stem}.ota"
    run_packer(build_dir, full, None)

    prev = previous_app(args.previous, args.board)
    if prev is not None:
        base, base_file = prev
        if base >= version:
            raise SystemExit(f"the previous release 0x{base:08X} is not older than 0x{version:08X}")
        # The packer takes the base by its version from its archive next to the build.
        keep = build_dir.parent.parent / "zigbee_ota"
        keep.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(base_file, keep / f"{name}-{base:08X}.bin")
        delta = out / f"{stem}-from-{base:08X}.ota"
        run_packer(build_dir, delta, base)
        entries.append(index_entry(delta, f"{url_base}/{delta.name}", base, args.notes_url))

    entries.append(index_entry(full, f"{url_base}/{full.name}", None, args.notes_url))
    for entry in entries:
        if entry["fileVersion"] != version:
            raise SystemExit(f"{entry['fileName']}: version 0x{entry['fileVersion']:08X}, expected 0x{version:08X}")
    part = out / f"index-{args.board}.json"
    part.write_text(json.dumps(entries, indent=2) + "\n")
    for entry in entries:
        kind = "delta" if "minFileVersion" in entry else "full"
        print(f"{entry['fileName']}: {kind}, {entry['fileSize']} B")
    return 0


def cmd_index(args) -> int:
    entries = []
    for part in args.parts:
        entries += json.loads(Path(part).read_text())
    # Deltas first, then full images; within each, the order of the parts.
    entries.sort(key=lambda e: "minFileVersion" not in e)
    seen = set()
    for e in entries:
        key = (e["manufacturerCode"], e["imageType"], e.get("minFileVersion"))
        if key in seen:
            raise SystemExit(f"two entries for image type 0x{e['imageType']:04X} from the same base")
        seen.add(key)
    with tempfile.NamedTemporaryFile("w", dir=args.out.parent, delete=False, suffix=".tmp") as tmp:
        tmp.write(json.dumps(entries, indent=2) + "\n")
    Path(tmp.name).chmod(0o644)
    Path(tmp.name).replace(args.out)
    print(f"{args.out}: {len(entries)} entries")
    return 0


# A static import, a re-export or a dynamic import with a fixed specifier.
JS_IMPORT = re.compile(r"""(?:\bfrom|\bimport)\s*\(?\s*["'`]([^"'`]+)["'`]""")


def npm_range_fits(version: str, wanted: str) -> bool:
    """version (x.y.z) fits wanted as npm reads it: an exact x.y.z or ^x.y.z."""
    def parts(v):
        m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", v)
        if m is None:
            raise SystemExit(f"{v}: not an x.y.z version, check it by hand")
        return tuple(int(p) for p in m.groups())
    if not wanted.startswith("^"):
        return parts(version) == parts(wanted)
    have, low = parts(version), parts(wanted[1:])
    if have < low:
        return False
    if low[0] > 0:
        return have[0] == low[0]
    if low[1] > 0:
        return have[:2] == low[:2]
    return have == low


def cmd_site(args) -> int:
    images = {}
    for path in args.images:
        m = re.fullmatch(r"hiflow-zb-(.+)-[0-9A-Fa-f]{8}\.factory\.bin", path.name)
        if m is None or m.group(1) not in BOARDS:
            raise SystemExit(f"{path.name}: not the factory image of a board")
        if m.group(1) in images:
            raise SystemExit(f"two factory images for {m.group(1)}")
        images[m.group(1)] = path
    missing = sorted(set(BOARDS) - set(images))
    if missing:
        raise SystemExit(f"no factory image for {', '.join(missing)}")
    tools = {
        # site path: source
        "esp-web-tools/flash.js": args.esp_web_tools / "dist" / "flash.js",
        "esp-web-tools/util/sleep.js": args.esp_web_tools / "dist" / "util" / "sleep.js",
        "esp-web-tools/LICENSE": args.esp_web_tools / "LICENSE",
        "esptool-js/bundle.js": args.esptool_js / "bundle.js",
        "esptool-js/LICENSE": args.esptool_js / "LICENSE",
    }
    for src in tools.values():
        if not src.is_file():
            raise SystemExit(f"{src}: missing, not the npm package folder of ESP Web Tools or esptool-js")
    # esptool-js has its own pin: it must fit the range ESP Web Tools names.
    wanted = json.loads((args.esp_web_tools / "package.json").read_text()).get("dependencies", {}).get("esptool-js")
    have = json.loads((args.esptool_js / "package.json").read_text())["version"]
    if wanted is None or not npm_range_fits(have, wanted):
        raise SystemExit(f"esptool-js {have} does not fit the range ESP Web Tools names ({wanted})")

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    for board, path in images.items():
        manifest = web_manifest(board, path, args.tag)
        shutil.copyfile(path, out / path.name)
        (out / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    page = (WEB / "index.html").read_text()
    for key, value in (("TAG", args.tag), ("REPO_URL", args.repo_url.rstrip("/"))):
        page = page.replace(f"@{key}@", html.escape(value))
    left = re.search(r"@[A-Z_]+@", page)
    if left:
        raise SystemExit(f"web/index.html: no value for {left.group(0)}")
    (out / "index.html").write_text(page)
    (out / "img").mkdir(exist_ok=True)
    for picture in (ROOT / "docs" / "img").glob("board-*.jpg"):
        shutil.copyfile(picture, out / "img" / picture.name)
    for dst, src in tools.items():
        (out / dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, out / dst)
    # The page loads flash() through its import map: every target must be there.
    imports = json.loads(re.search(r'<script type="importmap">(.*?)</script>', page, re.S).group(1))["imports"]
    for target in imports.values():
        if not (out / target).is_file():
            raise SystemExit(f"web/index.html: the import map names {target}, which the site lacks")
    # Every import of flash() and the modules it loads must find a file on the site.
    todo, seen = ["esp-web-tools/flash.js"], set()
    while todo:
        module = todo.pop()
        if module in seen:
            continue
        seen.add(module)
        for spec in JS_IMPORT.findall((out / module).read_text()):
            if spec.startswith(("./", "../")):
                key = "./" + posixpath.normpath(posixpath.join(posixpath.dirname(module), spec))
                target = imports.get(key, key)
            else:
                target = imports.get(spec)
            target = None if target is None else posixpath.normpath(target)
            if target is None or target.startswith("../") or not (out / target).is_file():
                raise SystemExit(f"{module}: its import {spec!r} finds no file on the site")
            todo.append(target)
    print(f"{out}: installer for {args.tag}, {len(images)} boards")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("version")
    v.add_argument("--date")
    v.add_argument("--index", action="append")
    p = sub.add_parser("pack")
    p.add_argument("board")
    p.add_argument("build_dir", type=Path)
    p.add_argument("--url-base", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--previous", type=Path)
    p.add_argument("--notes-url")
    i = sub.add_parser("index")
    i.add_argument("--out", type=Path, required=True)
    i.add_argument("parts", nargs="+")
    s = sub.add_parser("site")
    s.add_argument("--out", type=Path, required=True)
    s.add_argument("--tag", required=True)
    s.add_argument("--repo-url", required=True)
    s.add_argument("--esp-web-tools", type=Path, required=True)
    s.add_argument("--esptool-js", type=Path, required=True)
    s.add_argument("images", nargs="+", type=Path)
    args = ap.parse_args()
    return {"version": cmd_version, "pack": cmd_pack, "index": cmd_index, "site": cmd_site}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
