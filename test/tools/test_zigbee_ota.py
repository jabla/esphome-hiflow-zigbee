"""Tests for the OTA file versions (components/zigbee_ota/version.py) and the
packer (tools/make_zigbee_ota.py).

Run: python3 -m unittest discover -s test/tools
"""
import contextlib
import datetime
import hashlib
import importlib.util
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


versions = load("zigbee_ota_version", ROOT / "components" / "zigbee_ota" / "version.py")
packer = load("make_zigbee_ota", ROOT / "tools" / "make_zigbee_ota.py")

DAY = datetime.date(2027, 3, 14)
LEGACY = 0x6ABE65BB  # a Unix-time version from before the date versions


class VersionTest(unittest.TestCase):
    def test_encode_reads_as_the_date(self):
        self.assertEqual(versions.encode(DAY, 2), 0x27031402)
        self.assertEqual(versions.decode(0x27031402), (DAY, 2))
        self.assertEqual(versions.describe(0x27031402), "2027-03-14 build 2")

    def test_later_builds_and_days_are_newer(self):
        self.assertLess(versions.encode(DAY, 99), versions.encode(DAY + datetime.timedelta(days=1), 1))
        self.assertLess(versions.encode(datetime.date(2027, 12, 31), 99), versions.encode(datetime.date(2028, 1, 1), 1))
        self.assertLess(versions.encode(DAY, 9), versions.encode(DAY, 10))

    def test_legacy_versions_are_not_date_versions(self):
        for v in (LEGACY, 0x6AC0CCB5, 0x6ABBD429, 0x00000000, 0x27131401, 0x27023001, 0x27031400):
            self.assertFalse(versions.is_date_version(v), hex(v))
        self.assertEqual(versions.describe(LEGACY), "0x6ABE65BB")

    def test_next_version_counts_the_day(self):
        self.assertEqual(versions.next_version(DAY, []), 0x27031401)
        self.assertEqual(versions.next_version(DAY, [0x27031401, 0x27031403]), 0x27031404)
        # other days and legacy versions do not count
        self.assertEqual(versions.next_version(DAY, [0x27031309, LEGACY]), 0x27031401)

    def test_next_version_stops_after_99(self):
        with self.assertRaises(ValueError):
            versions.next_version(DAY, [0x27031499])

    def test_encode_rejects_out_of_range(self):
        for build in (0, 100):
            with self.assertRaises(ValueError):
                versions.encode(DAY, build)


class HeaderTest(unittest.TestCase):
    def test_no_base_uses_own_version_and_type(self):
        self.assertEqual(packer.header_for(0x27031401, 0x4857, None), (0x27031401, 0x4857, False))

    def test_legacy_device_gets_the_transition_header(self):
        self.assertEqual(packer.header_for(0x27031401, 0x4857, LEGACY),
                         (LEGACY + 1, packer.LEGACY_IMAGE_TYPE, True))

    def test_newer_than_installed(self):
        self.assertEqual(packer.header_for(0x27031402, 0x4857, 0x27031401), (0x27031402, 0x4857, False))

    def test_not_newer_than_installed_is_refused(self):
        for installed in (0x27031401, 0x27031402):
            with self.assertRaises(SystemExit):
                packer.header_for(0x27031401, 0x4857, installed)


class SummaryTest(unittest.TestCase):
    def test_with_and_without_git(self):
        info = {"label": "hiflow-zb xiao_esp32c6", "git": "v0.5.1-2-g9c16845-dirty"}
        self.assertEqual(packer.summary(info, 0x27031401, False),
                         "2027-03-14 build 1, hiflow-zb xiao_esp32c6, v0.5.1-2-g9c16845-dirty")
        self.assertEqual(packer.summary({"label": "x", "git": None}, 0x27031401, False), "2027-03-14 build 1, x")
        self.assertEqual(packer.summary({}, 0x27031401, False), "2027-03-14 build 1")

    def test_transition_and_length(self):
        self.assertTrue(packer.summary({}, 0x27031401, True).startswith("Moves to date versions: "))
        self.assertLessEqual(len(packer.summary({"label": "x" * 400}, 0x27031401, False)), packer.SUMMARY_MAX)


CHIP_C6, CHIP_H2 = 13, 16


def fake_image(size: int = 4096, seed: int = 0, chip: int = CHIP_C6) -> bytes:
    """Enough of an ESP app image for the packer: magic, chip id, hash-appended
    flag, a body and a trailing SHA-256."""
    body = bytearray(hashlib.sha256(str(seed).encode()).digest() * (size // 32))
    body[0] = packer.ESP_IMAGE_MAGIC
    struct.pack_into("<H", body, 12, chip)
    body[23] = 1
    return bytes(body) + hashlib.sha256(body).digest()


class PackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.keep = self.root / ".esphome" / "zigbee_ota"
        self.build_dir = self.root / ".esphome" / "build" / "hiflow-zb"

    def tearDown(self):
        self.tmp.cleanup()

    def compile(self, version: int, image_type: int = 0x4857, seed: int = 0, chip: int = CHIP_C6,
                **extra) -> bytes:
        (self.build_dir / "build").mkdir(parents=True, exist_ok=True)
        info = {"file_version": version, "manufacturer_code": 0x131B, "image_type": image_type, **extra}
        (self.build_dir / "zigbee_ota.json").write_text(json.dumps(info))
        image = fake_image(seed=seed, chip=chip)
        firmware = self.build_dir / "build" / "firmware.ota.bin"
        firmware.write_bytes(image)
        # the image is written after the json, as in a real compile
        stat = (self.build_dir / "zigbee_ota.json").stat()
        os.utime(firmware, (stat.st_atime + 1, stat.st_mtime + 1))
        return image

    def pack(self, *args: str) -> int:
        argv = ["make_zigbee_ota.py", str(self.build_dir), *args]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()):
            return packer.main()

    def header(self, path: Path) -> tuple[int, int, int]:
        """manufacturer, image type, version from an OTA file header"""
        return struct.unpack_from("<HHI", path.read_bytes(), 10)

    def index(self) -> list[dict]:
        return json.loads((self.keep / "index.json").read_text())["firmwares"]

    def test_full_image_and_index(self):
        self.compile(0x27031401, label="hiflow-zb waveshare_c6_lcd147", git="v0.5.1")
        self.assertEqual(self.pack(), 0)
        ota = self.keep / "hiflow-zb-27031401.ota"
        self.assertEqual(self.header(ota), (0x131B, 0x4857, 0x27031401))
        self.assertTrue((self.keep / "hiflow-zb-27031401.bin").is_file())
        [fw] = self.index()
        self.assertEqual(fw["path"], ota.name)
        self.assertEqual(fw["file_version"], 0x27031401)
        self.assertEqual(fw["image_type"], 0x4857)
        self.assertEqual(fw["file_size"], ota.stat().st_size)
        self.assertEqual(fw["checksum"], "sha3-256:" + hashlib.sha3_256(ota.read_bytes()).hexdigest())
        self.assertEqual(fw["changelog"], "2027-03-14 build 1, hiflow-zb waveshare_c6_lcd147, v0.5.1")
        self.assertNotIn("release_notes", fw)

    def test_notes_and_index_replaces_same_type_only(self):
        self.compile(0x27031401)
        self.pack()
        self.compile(0x27031401, image_type=0x4858)
        self.pack()
        self.compile(0x27031402)
        self.pack("--from", "0x27031401", "--notes", "Display at 15 %")
        by_type = {fw["image_type"]: fw for fw in self.index()}
        self.assertEqual(sorted(by_type), [0x4857, 0x4858])
        self.assertEqual(by_type[0x4857]["file_version"], 0x27031402)
        self.assertEqual(by_type[0x4857]["release_notes"], "Display at 15 %")
        self.assertEqual(by_type[0x4858]["file_version"], 0x27031401)

    def test_broken_index_is_started_anew(self):
        self.keep.mkdir(parents=True)
        (self.keep / "index.json").write_text("{not json")
        self.compile(0x27031401)
        self.assertEqual(self.pack(), 0)
        self.assertEqual(len(self.index()), 1)

    def test_transition_from_a_legacy_device(self):
        self.compile(0x27031401)
        self.assertEqual(self.pack("--from", f"0x{LEGACY:08X}"), 0)
        # no kept legacy image here: full image; one file made for the old
        # firmware, one with the board's type for the updated device
        moving = self.keep / f"hiflow-zb-27031401-from-{LEGACY:08X}.ota"
        regular = self.keep / "hiflow-zb-27031401.ota"
        self.assertEqual(self.header(moving), (0x131B, packer.LEGACY_IMAGE_TYPE, LEGACY + 1))
        self.assertEqual(self.header(regular), (0x131B, 0x4857, 0x27031401))
        self.assertEqual(moving.read_bytes()[packer.HEADER_LENGTH:], regular.read_bytes()[packer.HEADER_LENGTH:])
        by_type = {fw["image_type"]: fw for fw in self.index()}
        self.assertEqual(by_type[packer.LEGACY_IMAGE_TYPE]["file_version"], LEGACY + 1)
        self.assertEqual(by_type[packer.LEGACY_IMAGE_TYPE]["path"], moving.name)
        self.assertTrue(by_type[packer.LEGACY_IMAGE_TYPE]["changelog"].startswith("Moves to date versions: 2027-03-14 build 1"))
        self.assertEqual(by_type[0x4857]["file_version"], 0x27031401)
        self.assertEqual(by_type[0x4857]["path"], regular.name)

    def test_older_than_installed_is_refused(self):
        self.compile(0x27031401)
        with self.assertRaises(SystemExit):
            self.pack("--from", "0x27031402")
        self.assertFalse((self.keep / "hiflow-zb-27031401.ota").exists())

    def test_unfinished_compile_is_refused(self):
        self.compile(0x27031401)
        firmware = self.build_dir / "build" / "firmware.ota.bin"
        os.utime(firmware, (0, 0))
        self.assertEqual(self.pack(), 1)

    @unittest.skipUnless(importlib.util.find_spec("detools"), "detools not installed")
    def test_delta_against_the_kept_image(self):
        self.compile(0x27031401, seed=1)
        self.pack()
        new = self.compile(0x27031402, seed=2)
        self.assertEqual(self.pack("--from", "0x27031401"), 0)
        data = (self.keep / "hiflow-zb-27031402.ota").read_bytes()
        tag, = struct.unpack_from("<H", data, packer.HEADER_LENGTH)
        self.assertEqual(tag, packer.TAG_IMAGE_DELTA)
        self.assertEqual((self.keep / "hiflow-zb-27031402.bin").read_bytes(), new)

    def sub_element(self, path: Path) -> tuple[int, bytes]:
        """tag and data of the one sub-element after the header"""
        data = path.read_bytes()
        tag, length = struct.unpack_from("<HI", data, packer.HEADER_LENGTH)
        return tag, data[packer.HEADER_LENGTH + 6:packer.HEADER_LENGTH + 6 + length]

    def test_full_image_for_a_c6_is_zlib(self):
        image = self.compile(0x27031401)
        self.assertEqual(self.pack(), 0)
        tag, body = self.sub_element(self.keep / "hiflow-zb-27031401.ota")
        self.assertEqual(tag, packer.TAG_IMAGE_ZLIB)
        self.assertEqual(zlib.decompress(body), image)

    @unittest.skipUnless(importlib.util.find_spec("detools"), "detools not installed")
    def test_full_image_for_an_h2_is_heatshrink(self):
        import detools
        image = self.compile(0x27031401, image_type=0x4832, chip=CHIP_H2)
        self.assertEqual(self.pack(), 0)
        tag, body = self.sub_element(self.keep / "hiflow-zb-27031401.ota")
        self.assertEqual(tag, packer.TAG_IMAGE_HEATSHRINK)
        out = io.BytesIO()
        detools.apply_patch(io.BytesIO(b""), io.BytesIO(body), out)
        self.assertEqual(out.getvalue(), image)

    def test_full_image_for_an_h2_needs_detools(self):
        self.compile(0x27031401, image_type=0x4832, chip=CHIP_H2)
        with mock.patch.dict(sys.modules, {"detools": None}), self.assertRaises(SystemExit):
            self.pack()
        self.assertFalse((self.keep / "hiflow-zb-27031401.ota").exists())

    @unittest.skipUnless(importlib.util.find_spec("detools"), "detools not installed")
    def test_delta_against_another_chip_is_refused(self):
        self.compile(0x27031401, seed=1)
        self.pack()
        self.compile(0x27031402, seed=2, chip=CHIP_H2)
        self.assertEqual(self.pack("--from", "0x27031401"), 1)
        self.assertFalse((self.keep / "hiflow-zb-27031402.ota").exists())


if __name__ == "__main__":
    unittest.main()
