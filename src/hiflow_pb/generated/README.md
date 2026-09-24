# Generated nanopb C sources

nanopb-generated decoders/encoders for the HiFlow protobuf messages, plus the
`.proto`/`.options` inputs they were generated from.

| file | content |
|---|---|
| `RealDataNew.proto` / `.options` | verbatim copy of `test/ref/hiflow_ble/protobuf/RealDataNew.proto` + nanopb max_count/max_size options |
| `RealDataNew.pb.{c,h}` | `RealDataNewReqDTO`, `SGSMO`, `PvMO`, `MeterMO`, `RpMO`, `RSDMO`, `TGSMO`, `RealDataNewResDTO` |
| `CommandPB.proto` / `.options` | verbatim copy of `test/ref/hiflow_ble/protobuf/CommandPB.proto` + options |
| `CommandPB.pb.{c,h}` | `CommandReqDTO`, `CommandResDTO`, `CommandStatusReqDTO`, `CommandStatusResDTO`, … (0xA305 control-command path) |
| `CommCmdPB.proto` / `.options` | **hand-authored** CommCmd handshake messages (see below) |
| `CommCmdPB.pb.{c,h}` | `CommCmdResDTO`, `CommCmdStatusResDTO`, `CommCmdStatusReqDTO` |

## Generator

nanopb generator **0.4.9.2** (same version as the vendored runtime in
`../nanopb/`). The generator is run from the released checkout, not from PyPI,
so generator and runtime versions match exactly:

```sh
git clone https://github.com/nanopb/nanopb.git /tmp/nanopb-src
git -C /tmp/nanopb-src checkout nanopb-0.4.9.2
uv venv /tmp/pbvenv
uv pip install --python /tmp/pbvenv/bin/python protobuf

cd src/hiflow_pb/generated
for p in RealDataNew.proto CommandPB.proto CommCmdPB.proto; do
  /tmp/pbvenv/bin/python /tmp/nanopb-src/generator/nanopb_generator.py "$p"
done
```

(`../regen.sh` runs exactly this.)

## Notes on field types

* `SGSMO.voltage/frequency/active_power/temperature` and
  `PvMO.voltage/current/power/energy_total/energy_daily/error_code` are `int32`
  in the upstream `.proto`, so nanopb maps them to `int32_t`. The largest value
  in the corpus, `error_code = 50331648`, is well below `2^31` and round-trips
  losslessly. `serial_number` is `int64` and maps to `int64_t`.
* Repeated fields get an explicit `max_count`, strings an explicit `max_size`,
  because the embedded build uses nanopb **static allocation** (no malloc).

## CommCmd handshake messages — why a new `.proto`

`CommCmdResDTO` / `CommCmdStatusResDTO` / `CommCmdStatusReqDTO` do **not** exist
in any upstream `.proto` file. TheTiEr/hiflow-ble encodes and parses them by
hand in `hiflow_ble/hiflow.py`. `CommCmdPB.proto` reproduces the field numbers
from that code (documented in the file header). Field layout:

| message | fields |
|---|---|
| `CommCmdResDTO` (app to device, cmd `0xA318`) | `1 time`, `2 action`, `5 tid`, `6 data` |
| `CommCmdStatusResDTO` (app to device poll, cmd `0xA319`) | `1 time`, `2 action`, `4 tid` |
| `CommCmdStatusReqDTO` (device to app) | `3 action`, `11 sts` |

Action codes: **64** = login (`data` = bleId string), **82** = submit PIN
(`data` = PIN string, only after a login `sts==3`), **104** = time-sync
(`data` = `"<unix_timestamp>,<tz_offset_seconds>\r"`).

Status semantics (`sts`): action 64: 0=in-progress, 1=OK, 3=PIN-needed;
action 82: 0=SUCCESS, 1=wrong-PIN; action 104: 0=OK.

No credentials are stored anywhere in this repository; bleId / PIN / timezone
offset are supplied by the caller at run time.

Note: the "handshake-like" plaintexts in `test/vectors.json` (`v1_frames`) are
synthetic frame-crypto vectors, not real handshake messages, so they are not
decoded against `CommCmdPB`.
