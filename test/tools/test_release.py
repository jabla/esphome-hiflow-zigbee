"""Tests for the release files (tools/release.py): versions, index entries, the
previous release's image, one index for all boards, the web installer.

Run: python3 -m unittest discover -s test/tools
"""
import argparse
import contextlib
import importlib.util
import io
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load("release", ROOT / "tools" / "release.py")
packer = load("make_zigbee_ota_for_release", ROOT / "tools" / "make_zigbee_ota.py")


class ReleaseVersionTest(unittest.TestCase):
    def run_version(self, *indexes):
        with tempfile.TemporaryDirectory() as d:
            files = []
            for i, found in enumerate(indexes):
                f = Path(d) / f"index-{i}.json"
                f.write_text(json.dumps([{"fileVersion": v} for v in found]))
                files.append(str(f))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                release.cmd_version(argparse.Namespace(date="2027-03-14", index=files))
            return out.getvalue().strip()

    def test_first_release_of_a_day_is_build_1(self):
        self.assertEqual(self.run_version(), "0x27031401")

    def test_a_second_release_of_the_day_counts_on(self):
        self.assertEqual(self.run_version([0x27031401], [0x27031402]), "0x27031403")

    def test_other_days_do_not_count(self):
        self.assertEqual(self.run_version([0x27031305]), "0x27031401")

    def test_no_build_left(self):
        with self.assertRaises(SystemExit):
            self.run_version([0x27031499])


class IndexEntryTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "x.ota"
        self.path.write_bytes(packer.build(0xF100, b"payload", 0x131B, 0x4858, 0x27031401, "hiflow-zb xiao"))

    def tearDown(self):
        self.dir.cleanup()

    def test_full_image(self):
        e = release.index_entry(self.path, "https://example.org/x.ota", None, None)
        self.assertEqual((e["manufacturerCode"], e["imageType"], e["fileVersion"]), (0x131B, 0x4858, 0x27031401))
        self.assertEqual(e["fileSize"], self.path.stat().st_size)
        self.assertEqual(len(e["sha512"]), 128)
        self.assertEqual(e["modelId"], "HMS-2000-4WB Bridge")
        self.assertEqual(e["manufacturerName"], ["esphome"])
        self.assertEqual(e["otaHeaderString"], "hiflow-zb xiao")
        self.assertNotIn("minFileVersion", e)
        self.assertNotIn("releaseNotes", e)

    def test_delta_only_for_its_base(self):
        e = release.index_entry(self.path, "https://example.org/x.ota", 0x27031301, "https://example.org/notes")
        self.assertEqual((e["minFileVersion"], e["maxFileVersion"]), (0x27031301, 0x27031301))
        self.assertEqual(e["releaseNotes"], "https://example.org/notes")

    def test_not_an_ota_file(self):
        self.path.write_bytes(b"\xe9" + bytes(100))
        with self.assertRaises(SystemExit):
            release.index_entry(self.path, "https://example.org/x.ota", None, None)


class PreviousAppTest(unittest.TestCase):
    def test_newest_image_of_the_board(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("hiflow-zb-xiao_esp32c6-27031201.app.bin", "hiflow-zb-xiao_esp32c6-27031301.app.bin",
                         "hiflow-zb-waveshare_h2_zero-27031401.app.bin", "hiflow-zb-xiao_esp32c6-27031301.ota"):
                (Path(d) / name).write_bytes(b"x")
            version, path = release.previous_app(Path(d), "xiao_esp32c6")
            self.assertEqual(version, 0x27031301)
            self.assertEqual(path.name, "hiflow-zb-xiao_esp32c6-27031301.app.bin")
            self.assertIsNone(release.previous_app(Path(d), "waveshare_c6_lcd147"))

    def test_no_previous_release(self):
        self.assertIsNone(release.previous_app(None, "xiao_esp32c6"))


class IndexTest(unittest.TestCase):
    @staticmethod
    def entry(image_type, base=None):
        e = {"fileVersion": 0x27031401, "manufacturerCode": 0x131B, "imageType": image_type, "fileName": f"{image_type:x}"}
        if base is not None:
            e["minFileVersion"] = e["maxFileVersion"] = base
        return e

    def run_index(self, parts):
        with tempfile.TemporaryDirectory() as d:
            files = []
            for i, part in enumerate(parts):
                f = Path(d) / f"index-{i}.json"
                f.write_text(json.dumps(part))
                files.append(str(f))
            out = Path(d) / "index.json"
            release.cmd_index(argparse.Namespace(out=out, parts=files))
            return json.loads(out.read_text())

    def test_deltas_first(self):
        merged = self.run_index([[self.entry(0x4858, 0x27031301), self.entry(0x4858)],
                                 [self.entry(0x4857, 0x27031301), self.entry(0x4857)]])
        self.assertEqual([("minFileVersion" in e, e["imageType"]) for e in merged],
                         [(True, 0x4858), (True, 0x4857), (False, 0x4858), (False, 0x4857)])

    def test_two_boards_with_one_image_type_are_refused(self):
        with self.assertRaises(SystemExit):
            self.run_index([[self.entry(0x4858)], [self.entry(0x4858)]])


class WebInstallerTest(unittest.TestCase):
    CHIPS = {"waveshare_c6_lcd147": 13, "xiao_esp32c6": 13, "waveshare_h2_zero": 16}

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        # The npm package folders the site takes its files from.
        self.tools = self.root / "ewt"
        (self.tools / "dist" / "util").mkdir(parents=True)
        self.flash_js = 'import { ESPLoader } from "esptool-js";\nimport { sleep } from "./util/sleep";\n'
        (self.tools / "dist" / "flash.js").write_text(self.flash_js)
        (self.tools / "dist" / "util" / "sleep.js").write_text("export const sleep = 0;\n")
        (self.tools / "LICENSE").write_text("Apache-2.0")
        (self.tools / "package.json").write_text(json.dumps({"dependencies": {"esptool-js": "^0.6.0"}}))
        self.esptool = self.root / "esptool-js"
        self.esptool.mkdir()
        (self.esptool / "bundle.js").write_text("// esptool stub")
        (self.esptool / "LICENSE").write_text("Apache-2.0")
        (self.esptool / "package.json").write_text(json.dumps({"version": "0.6.1"}))

    def tearDown(self):
        self.dir.cleanup()

    def factory(self, board, chip_id):
        # The bootloader header (esp_image_header_t) up to the chip id, then filler.
        path = self.root / f"hiflow-zb-{board}-27031401.factory.bin"
        path.write_bytes(b"\xe9\x03" + bytes(10) + struct.pack("<H", chip_id) + bytes(64))
        return path

    def run_site(self, images):
        out = self.root / "site"
        release.cmd_site(argparse.Namespace(out=out, tag="v1.2.3", repo_url="https://github.com/o/r/",
                                            esp_web_tools=self.tools, esptool_js=self.esptool, images=images))
        return out

    def test_site(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        out = self.run_site(images)
        page = (out / "index.html").read_text()
        self.assertNotRegex(page, r"@[A-Z_]+@")
        self.assertIn('href="https://github.com/o/r/releases/tag/v1.2.3"', page)
        for board, chip_id in self.CHIPS.items():
            m = json.loads((out / f"manifest-{board}.json").read_text())
            self.assertEqual(m["builds"][0]["chipFamily"], "ESP32-H2" if chip_id == 16 else "ESP32-C6")
            part = m["builds"][0]["parts"]
            self.assertEqual(part, [{"path": f"hiflow-zb-{board}-27031401.factory.bin", "offset": 0}])
            self.assertTrue((out / part[0]["path"]).is_file())
            self.assertEqual(m["version"], "v1.2.3")
        for src in re.findall(r'src="([^"]+)"', page):
            self.assertTrue((out / src).is_file(), src)
        self.assertEqual((out / "esp-web-tools" / "flash.js").read_text(), self.flash_js)
        self.assertEqual((out / "esptool-js" / "bundle.js").read_text(), "// esptool stub")
        for lic in ("esp-web-tools/LICENSE", "esptool-js/LICENSE"):
            self.assertTrue((out / lic).is_file(), lic)
        # flash() itself, which the page imports through the import map
        self.assertIn('import("./esp-web-tools/flash.js")', page)

    def test_an_esptool_js_outside_esp_web_tools_range_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        (self.esptool / "package.json").write_text(json.dumps({"version": "0.7.0"}))
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_an_import_without_a_file_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        (self.tools / "dist" / "flash.js").write_text(self.flash_js + 'import { x } from "./util/new";\n')
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_an_import_one_module_deep_without_a_file_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        (self.tools / "dist" / "util" / "sleep.js").write_text('import "./gone";\n')
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_an_import_outside_the_site_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        (self.root / "x.js").write_text("// next to the site, not on it")
        (self.tools / "dist" / "flash.js").write_text(self.flash_js + 'import "../../x.js";\n')
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_npm_ranges(self):
        fits = [("0.6.1", "^0.6.0"), ("0.6.0", "^0.6.0"), ("1.9.0", "^1.2.3"), ("0.0.3", "^0.0.3"), ("2.1.0", "2.1.0")]
        misses = [("0.7.0", "^0.6.0"), ("0.5.9", "^0.6.0"), ("2.0.0", "^1.2.3"), ("0.0.4", "^0.0.3"), ("2.1.1", "2.1.0")]
        for version, wanted in fits:
            self.assertTrue(release.npm_range_fits(version, wanted), (version, wanted))
        for version, wanted in misses:
            self.assertFalse(release.npm_range_fits(version, wanted), (version, wanted))
        # Ranges the check does not read are refused, never passed.
        for wanted in ("~0.6.0", ">=0.6.0", "^0.6.0 || ^0.7.0", "0.6.x", "^0.6.0-beta.1"):
            with self.assertRaises(SystemExit):
                release.npm_range_fits("0.6.1", wanted)

    @unittest.skipUnless((ROOT / "web" / "node_modules" / "esp-web-tools").is_dir(),
                         "needs npm ci in web/ (CI runs it)")
    def test_the_site_with_the_pinned_packages(self):
        modules = ROOT / "web" / "node_modules"
        self.tools, self.esptool = modules / "esp-web-tools", modules / "esptool-js"
        out = self.run_site([self.factory(b, c) for b, c in self.CHIPS.items()])
        self.assertTrue((out / "esptool-js" / "bundle.js").is_file())

    def test_a_missing_esptool_js_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        (self.esptool / "bundle.js").unlink()
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_an_image_for_another_chip_is_refused(self):
        images = [self.factory(b, 16) for b in self.CHIPS]
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_not_a_factory_image(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items()]
        images[0].write_bytes(bytes(100))
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_a_missing_board_is_refused(self):
        images = [self.factory(b, c) for b, c in self.CHIPS.items() if b != "waveshare_h2_zero"]
        with self.assertRaises(SystemExit):
            self.run_site(images)

    def test_the_boards_are_the_release_matrix(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text()
        matrix = re.search(r"board: \[([^\]]+)\]", workflow).group(1)
        self.assertEqual({b.strip() for b in matrix.split(",")}, set(release.BOARDS))

    def test_the_page_offers_every_board(self):
        page = (ROOT / "web" / "index.html").read_text()
        self.assertEqual(set(re.findall(r'<input type="radio" name="board" value="([^"]+)">', page)),
                         set(release.BOARDS))
        # and knows each board's chip, for the message about another chip
        for board, (_, chip) in release.BOARDS.items():
            self.assertRegex(page, board + r': \{ name: "an? [^"]+", chip: "' + chip + r'" \}')


if __name__ == "__main__":
    unittest.main()
