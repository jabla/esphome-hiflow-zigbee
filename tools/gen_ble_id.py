#!/usr/bin/env python3
"""Print a new bleId: the identity the bridge logs in with.

The inverter whitelists a bleId once the BLE PIN has been accepted for it, so
generate one, put it into hiflow_secrets.yaml and keep it. Any stable 17-18
digit decimal string works; this follows the S-Miles app's algorithm
(`com.hoymiles.utils.BleIdUtil.b()`), ported from `generate_ble_id()` in
TheTiEr/hiflow-ble (MIT).

Usage: python3 tools/gen_ble_id.py
"""
import hashlib
import time
import uuid

# Column-first permutation of 30 digit slots.
PERM = [
    0, 5, 10, 15, 20, 25,
    1, 6, 11, 16, 21, 26,
    2, 7, 12, 17, 22, 27,
    3, 8, 13, 18, 23, 28,
    4, 9, 14, 19, 24, 29,
]


def generate_ble_id() -> str:
    raw = f"{int(time.time() * 1000)}{uuid.uuid4()}"
    digits = [int(c, 16) % 10 for c in hashlib.md5(raw.encode("utf-8")).hexdigest()]
    permuted = [digits[i] for i in PERM]
    return str(int("".join(str(d) for d in permuted[:18])))


if __name__ == "__main__":
    print(generate_ble_id())
