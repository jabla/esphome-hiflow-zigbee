"""File versions of the zigbee_ota images: the build date, readable in hex.

A version is 0xYYMMDDNN: year, month and day of the build and the build of
that day, each as two decimal digits, so 0x27031402 reads as the second build
on 14 March 2027. Home Assistant shows the version in hex, and in hex these
numbers grow with the date and the build count: a later build is always a
newer version, up to the end of 2099.

Images built before this scheme carried the Unix time of the build (0x6A...),
which is larger than any date version. Those versions always hold a hex digit
A-F, a date version never does, so the two cannot be mistaken for each other.

No ESPHome imports here: tools/make_zigbee_ota.py and the tests use this file
as it is.
"""

import datetime
from collections.abc import Iterable

MAX_BUILDS_PER_DAY = 99


def encode(day: datetime.date, build: int) -> int:
    if not 1 <= build <= MAX_BUILDS_PER_DAY:
        raise ValueError(f"build {build} of a day is outside 1-{MAX_BUILDS_PER_DAY}")
    if not 2000 <= day.year <= 2099:
        raise ValueError(f"year {day.year} does not fit the version")
    return int(f"{day.year % 100:02d}{day.month:02d}{day.day:02d}{build:02d}", 16)


def decode(version: int) -> tuple[datetime.date, int] | None:
    """The build date and the build of that day, or None for a version that is
    not a date version (an image from before the scheme)."""
    digits = f"{version:08x}"
    if len(digits) != 8 or not digits.isdigit():
        return None
    try:
        day = datetime.date(2000 + int(digits[0:2]), int(digits[2:4]), int(digits[4:6]))
    except ValueError:
        return None
    build = int(digits[6:8])
    if build < 1:
        return None
    return day, build


def is_date_version(version: int) -> bool:
    return decode(version) is not None


def next_version(day: datetime.date, known: Iterable[int]) -> int:
    """The version for a new build on `day`: one past the highest build of that
    day among `known` (earlier builds and kept images), or build 1."""
    builds = [d[1] for d in map(decode, known) if d is not None and d[0] == day]
    build = max(builds, default=0) + 1
    if build > MAX_BUILDS_PER_DAY:
        raise ValueError(
            f"already {MAX_BUILDS_PER_DAY} builds on {day.isoformat()}: the version "
            "has no room for another one today"
        )
    return encode(day, build)


def describe(version: int) -> str:
    """'2027-03-14 build 2' for a date version, the hex number otherwise."""
    d = decode(version)
    if d is None:
        return f"0x{version:08X}"
    return f"{d[0].isoformat()} build {d[1]}"
