#!/usr/bin/env python3
"""Keeps the minimum ESPHome in docs/own-build.md equal to the one the components require.

The components refuse older ESPHome releases with cv.require_esphome_version(...)
in components/*/__init__.py. They must all name the same release; docs/own-build.md
("What you need") states it. CI runs this script and fails on a diff, so raising
the minimum in the code without the doc (or the other way round) cannot land.
The CI job esphome-minimum builds the boards with the version --version prints.

Usage: python3 tools/sync_esphome_min.py            (rewrites the doc line)
       python3 tools/sync_esphome_min.py --version  (prints the minimum)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "own-build.md"
REQUIRE = re.compile(r"require_esphome_version\((\d+),\s*(\d+),\s*(\d+)\)")
DOC_LINE = re.compile(r"^(- ESPHome \*\*)(\d+\.\d+\.\d+)(\*\* or newer)", re.MULTILINE)


def minimum() -> str:
    found = {}
    for init in sorted(ROOT.glob("components/*/__init__.py")):
        for m in REQUIRE.finditer(init.read_text()):
            found.setdefault(".".join(m.groups()), []).append(init.parent.name)
    if len(found) != 1:
        sys.exit(f"components must require one ESPHome release, found: {found or 'none'}")
    return next(iter(found))


def main() -> None:
    version = minimum()
    if sys.argv[1:] == ["--version"]:
        print(version)
        return
    text = DOC.read_text()
    new, count = DOC_LINE.subn(rf"\g<1>{version}\g<3>", text)
    if count != 1:
        sys.exit(f"docs/own-build.md: expected one '- ESPHome **X.Y.Z** or newer' line, found {count}")
    if new != text:
        DOC.write_text(new)
        print(f"docs/own-build.md: minimum ESPHome set to {version}")


if __name__ == "__main__":
    main()
