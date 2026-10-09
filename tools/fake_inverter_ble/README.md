# Fake HiFlow inverter over BLE (bench)

A Linux laptop plays the inverter, so the bridge can run the real protocol on the bench
without a solar panel: V0 pairing, login and data every 30 s, for power and sleep
measurements, night mode tests (switch the fake off) and Zigbee OTA loops.

`fake_ble.py` runs a BlueZ GATT peripheral (service e0ff, ffe1 write, ffe2 notify) and hands
every frame to `test/host/fake_inverter.c`, the simulated inverter of the host tests, built as
`libfake.so` together with `shim.c`. It reports the recorded two-page reading of the host tests
(`test/vectors.json`).

Needs: Linux with BlueZ and a BLE adapter, `uv`, OpenSSL headers, a C compiler.

Build the library (from the repository root):

    make -C test/host libfake

Run it (one instance per adapter: two GATT servers on one adapter break the connection):

    cd tools/fake_inverter_ble && uv run --with 'bless==0.3.*' python fake_ble.py

Environment: `SN` (default `0000000000AA`), `PIN` (default none), `KEY` (the session key the V0
pairing hands out, 32 hex digits) and `CHUNK` (notification size, default 20 bytes; at most
the MTU minus 3).

Point a bridge at it with a secrets file of its own, e.g. a copy of
`hiflow_secrets.example.yaml` with:

- `hiflow_mac`: the laptop adapter's address (`bluetoothctl list`),
- `hiflow_sn: "0000000000AA"`,
- `hiflow_ble_id`: any, from `python3 tools/gen_ble_id.py`,
- `hiflow_ble_pin: ""`.

A file `power` next to the script sets the AC power the fake reports, in watts:
`echo 0 > power` is a dusk (the bridge sees standby after 5 minutes), removing the file goes
back to the recorded values. Only the power changes; serial and energies stay as recorded.
Stopping the script is the night.
