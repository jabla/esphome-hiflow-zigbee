#!/usr/bin/env python3
"""Generates reference test vectors for the C port of the HiFlow BLE protocol.

Uses the unmodified Python modules of the TheTiEr/hiflow-ble library (in
../ref/hiflow_ble) as the ground truth.

Usage:  python3 gen_vectors.py > vectors.json
"""
import datetime
import json
import sys
from pathlib import Path

REF = Path(__file__).resolve().parent / "ref"
sys.path.insert(0, str(REF))

from hiflow_ble import const as C  # noqa: E402
from hiflow_ble import crypt_util as CU  # noqa: E402
from hiflow_ble import frame as F  # noqa: E402

# Synthetic values! Real device data must NEVER go here: the file is versioned,
# and encRand/PIN/bleId are key material for the inverter.
ENC_RAND = bytes.fromhex("00112233445566778899aabbccddeeff")
SN = "0000000000AA"  # 12-char serial tail from the BLE name RMI-XXXXXXXXXXXX
PIN = "00000000"
BLE_ID = "1"
MAC = "AA:BB:CC:DD:EE:FF"

out = {
    "meta": {
        "enc_rand_hex": ENC_RAND.hex(),
        "sn": SN,
        "service_uuid": C.SERVICE_UUID,
        "tx_uuid": C.TX_UUID,
        "rx_uuid": C.RX_UUID,
        "mtu": C.DEFAULT_MTU,
        "salt_v0": C.SALT_V0.decode(),
    },
    "crc16_modbus": [
        {"data_hex": b.hex(), "crc": F.crc16_modbus(b)}
        for b in [b"", b"\x00", b"\x01\x02\x03", b"HM", bytes(range(16)), b"The quick brown fox"]
    ],
    "v1_key_nonce_aad": [],
    "v1_frames": [],
    "v0_key_iv": [],
    "v0_frames": [],
    "cmds": {
        "app_info_req": int.from_bytes(C.CMD_APP_INFO_DATA_REQ_DTO, "big"),
        "comm_cmd_res": int.from_bytes(C.CMD_COMM_CMD_RES_DTO, "big"),
        "comm_cmd_status": int.from_bytes(C.CMD_COMM_CMD_STATUS_RES, "big"),
        "real_data_res": int.from_bytes(C.CMD_REAL_DATA_RES_DTO, "big"),
        "hb_req": int.from_bytes(C.CMD_HB_REQ_DTO, "big"),
    },
    "synthetic_inputs": {"pin": PIN, "ble_id": BLE_ID, "mac": MAC},  # placeholders, no real data
}

for cmd, tid in [(0xA319, 0), (0xA318, 1), (0xA303, 0x1234)]:
    out["v1_key_nonce_aad"].append({
        "cmd": cmd,
        "tid": tid,
        "key_hex": CU.derive_key(ENC_RAND).hex(),
        "nonce_hex": CU.derive_nonce(ENC_RAND, cmd, tid).hex(),
        "aad_hex": CU.aad(cmd, tid).hex(),
    })

plaintexts = [
    (0xA319, 0, bytes.fromhex("0801100022050a033132302804")),  # handshake-like payload
    (0xA318, 7, b"\x08\x40"),
    (0xA303, 0x1234, bytes(range(1, 40))),
]
for cmd, tid, pt in plaintexts:
    frm = F.build_frame(ENC_RAND, cmd, tid, pt)
    pcmd, ptid, ppt = F.parse_frame(ENC_RAND, frm)
    assert (pcmd, ptid, ppt) == (cmd, tid, pt), "round-trip mismatch"
    out["v1_frames"].append({
        "cmd": cmd,
        "tid": tid,
        "plaintext_hex": pt.hex(),
        "frame_hex": frm.hex(),
        "crc": F.crc16_modbus(frm[10:10 + (len(frm) - 26)]),
    })

for cmd, tid in [(0xA201, 0), (0x8901, 3)]:
    out["v0_key_iv"].append({
        "cmd": cmd,
        "tid": tid,
        "key_hex": CU.derive_v0_key(SN).hex(),
        "iv_hex": CU.derive_v0_iv(SN, cmd, tid).hex(),
    })

for cmd, tid, pt in [(0xA201, 0, bytes.fromhex("0a033132301001")), (0x8901, 3, bytes(range(32)))]:
    frm = F.build_frame_v0(SN, cmd, tid, pt)
    pcmd, ptid, ppt = F.parse_frame_v0(SN, frm)
    assert (pcmd, ptid, ppt) == (cmd, tid, pt), "v0 round-trip mismatch"
    out["v0_frames"].append({
        "cmd": cmd,
        "tid": tid,
        "plaintext_hex": pt.hex(),
        "frame_hex": frm.hex(),
    })

# RealDataNew protobuf vector (if the pb2 module imports)
try:
    from hiflow_ble.protobuf import RealDataNew_pb2 as PB

    def fill(variant, sgs_power, sgs_voltage, sgs_freq, sgs_temp, ports):
        m = PB.RealDataNewReqDTO()
        sgs = m.sgs_data.add()
        sgs.active_power = sgs_power
        sgs.voltage = sgs_voltage
        sgs.frequency = sgs_freq
        sgs.temperature = sgs_temp
        for (volt, cur, power, etot, eday, err) in ports:
            pv = m.pv_data.add()
            pv.voltage = volt
            pv.current = cur
            pv.power = power
            pv.energy_total = etot
            pv.energy_daily = eday
            pv.error_code = err
        return m

    msg = fill(0, 1234, 2311, 5002, 421, [
        (4001, 150, 600, 123456, 789, 0),
        (3987, 145, 578, 98765, 654, 0),
        (0, 0, 0, 5, 0, 50331648),
        (0, 0, 0, 0, 0, 0),
    ])
    raw = msg.SerializeToString()
    out["protobuf_realdata_new"] = {
        "fields": {
            "sgs_data": [{"active_power": 1234, "voltage": 2311, "frequency": 5002, "temperature": 421}],
            "pv_data": [
                {"voltage": 4001, "current": 150, "power": 600, "energy_total": 123456, "energy_daily": 789, "error_code": 0},
                {"voltage": 3987, "current": 145, "power": 578, "energy_total": 98765, "energy_daily": 654, "error_code": 0},
                {"voltage": 0, "current": 0, "power": 0, "energy_total": 5, "energy_daily": 0, "error_code": 50331648},
                {"voltage": 0, "current": 0, "power": 0, "energy_total": 0, "energy_daily": 0, "error_code": 0},
            ],
        },
        "wire_hex": raw.hex(),
        "scaling": {
            "active_power": "x0.1 W",
            "voltage": "x0.1 V",
            "current": "x0.01 A",
            "frequency": "x0.01 Hz",
            "temperature": "x0.1 C",
        },
    }
except Exception as exc:  # pragma: no cover
    out["protobuf_realdata_new"] = {"error": repr(exc)}

# ---------------------------------------------------------------------------
# Request payloads and device replies.
#
# The C port has to encode exactly what the reference library puts on the wire.
# The three CommCmd builders below are copied verbatim from hiflow_ble/hiflow.py
# in TheTiEr/hiflow-ble (not vendored under test/ref/)
# ("CommCmd handshake helpers"), with the wall-clock call replaced by an explicit
# timestamp so the vectors are reproducible.
# ---------------------------------------------------------------------------

REQ_TIME = 1774000000          # fixed "now" for every request vector
REQ_OFFSET = 7200              # CEST, so the vectors also cover summer time


def _pb_varint_encode(n: int) -> bytes:
    out = bytearray()
    while n > 0x7F:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n & 0x7F)
    return bytes(out)


def _pb_varint_field(field: int, n: int) -> bytes:
    return bytes([(field << 3) | 0]) + _pb_varint_encode(n)


def _pb_string_field(field: int, s: bytes | str) -> bytes:
    if isinstance(s, str):
        s = s.encode()
    return bytes([(field << 3) | 2]) + _pb_varint_encode(len(s)) + s


def build_comm_cmd_res(action: int, data: str | bytes = "", t: int = REQ_TIME) -> bytes:
    """CommCmdResDTO: field 1=time, 2=action, 5=tid, 6=data."""
    payload = _pb_varint_field(1, t) + _pb_varint_field(2, action) + _pb_varint_field(5, t)
    return payload + _pb_string_field(6, data)


def build_comm_cmd_status_res(action: int, t: int = REQ_TIME) -> bytes:
    """CommCmdStatusResDTO: field 1=time, 2=action, 4=tid."""
    return _pb_varint_field(1, t) + _pb_varint_field(2, action) + _pb_varint_field(4, t)


def build_comm_cmd_status_req(action: int, sts: int) -> bytes:
    """CommCmdStatusReqDTO (device -> app): field 3=action, 11=sts."""
    return _pb_varint_field(3, action) + _pb_varint_field(11, sts)


# Local time string the way the reference formats it (datetime.now()); the C port
# derives the same string from unix time + offset.
ymd = datetime.datetime.fromtimestamp(REQ_TIME + REQ_OFFSET, datetime.timezone.utc).strftime(
    "%Y-%m-%d %H:%M:%S"
)

out["request_payloads"] = {
    "time": REQ_TIME,
    "offset": REQ_OFFSET,
    "time_ymd_hms": ymd,
    "ble_id": BLE_ID,
    "pin": PIN,
    "login": {"action": 64, "hex": build_comm_cmd_res(64, BLE_ID).hex()},
    "pin_cmd": {"action": 82, "hex": build_comm_cmd_res(82, PIN).hex()},
    "time_sync": {
        "action": 104,
        "data": f"{REQ_TIME},{REQ_OFFSET}\r",
        "hex": build_comm_cmd_res(104, f"{REQ_TIME},{REQ_OFFSET}\r").hex(),
    },
    "status_poll": {
        "64": build_comm_cmd_status_res(64).hex(),
        "82": build_comm_cmd_status_res(82).hex(),
        "104": build_comm_cmd_status_res(104).hex(),
    },
}

out["status_replies"] = [
    {"action": a, "sts": s, "hex": build_comm_cmd_status_req(a, s).hex()}
    for a, s in [(64, 0), (64, 1), (64, 3), (82, 0), (82, 1), (104, 0)]
]

# Data request (0xA311) and V0 pairing request (0xA301), both built from the
# reference .proto messages exactly like hiflow.py does.
try:
    from hiflow_ble.protobuf import APPInfomationData_pb2 as APPPB
    from hiflow_ble.protobuf import RealDataNew_pb2 as PB2

    def real_data_request(cp: int) -> bytes:
        req = PB2.RealDataNewResDTO()
        req.time_ymd_hms = ymd.encode()
        req.offset = REQ_OFFSET
        req.time = REQ_TIME
        req.cp = cp
        return req.SerializeToString()

    app_info = APPPB.APPInfoDataResDTO()
    app_info.time_ymd_hms = ymd.encode()
    app_info.offset = REQ_OFFSET
    app_info.time = REQ_TIME

    out["request_payloads"]["real_data_new"] = {
        "cp0_hex": real_data_request(0).hex(),
        "cp1_hex": real_data_request(1).hex(),
    }
    out["request_payloads"]["app_info_v0"] = {"hex": app_info.SerializeToString().hex()}

    # Paged reply: ap=2, ports 1+2 on page 0 (with the AC block), ports 3+4 on
    # page 1. The session core has to merge both pages before publishing.
    def page(cp: int, ports, with_sgs: bool) -> bytes:
        m = PB2.RealDataNewReqDTO()
        m.device_serial_number = "TESTDTU00001"
        m.ap = 2
        m.cp = cp
        if with_sgs:
            sgs = m.sgs_data.add()
            sgs.active_power = 8123
            sgs.voltage = 2305
            sgs.current = 353
            sgs.frequency = 4998
            sgs.temperature = 372
        for (port, volt, cur, power, etot, eday) in ports:
            pv = m.pv_data.add()
            pv.port_number = port
            pv.voltage = volt
            pv.current = cur
            pv.power = power
            pv.energy_total = etot
            pv.energy_daily = eday
        return m.SerializeToString()

    def single_page(ports) -> bytes:
        m = PB2.RealDataNewReqDTO()
        m.device_serial_number = "TESTDTU00001"
        m.ap = 1
        m.cp = 0
        sgs = m.sgs_data.add()
        sgs.active_power = 8123
        sgs.voltage = 2305
        sgs.current = 353
        sgs.frequency = 4998
        sgs.temperature = 372
        for (port, volt, cur, power, etot, eday) in ports:
            pv = m.pv_data.add()
            pv.port_number = port
            pv.voltage = volt
            pv.current = cur
            pv.power = power
            pv.energy_total = etot
            pv.energy_daily = eday
        return m.SerializeToString()

    out["protobuf_realdata_new_pages"] = {
        "ap": 2,
        "single_page_hex": single_page(
            [(1, 4001, 150, 6005, 123456, 789), (2, 3987, 145, 5780, 98765, 654)]
        ).hex(),
        "page0_hex": page(0, [(1, 4001, 150, 6005, 123456, 789), (2, 3987, 145, 5780, 98765, 654)], True).hex(),
        "page1_hex": page(1, [(3, 3900, 140, 5460, 55555, 321), (4, 3800, 130, 4940, 44444, 210)], False).hex(),
        "merged": {
            "ac_power_w": 812.3,
            "ac_voltage_v": 230.5,
            "ac_current_a": 3.53,
            "ac_frequency_hz": 49.98,
            "temperature_c": 37.2,
            "energy_total_wh": 123456 + 98765 + 55555 + 44444,
            "energy_daily_wh": 789 + 654 + 321 + 210,
            "ports": [
                {"port": 1, "power_w": 600.5, "voltage_v": 400.1, "current_a": 1.5},
                {"port": 2, "power_w": 578.0, "voltage_v": 398.7, "current_a": 1.45},
                {"port": 3, "power_w": 546.0, "voltage_v": 390.0, "current_a": 1.4},
                {"port": 4, "power_w": 494.0, "voltage_v": 380.0, "current_a": 1.3},
            ],
        },
    }
except Exception as exc:  # pragma: no cover
    out["request_payloads"]["error"] = repr(exc)

print(json.dumps(out, indent=2))
