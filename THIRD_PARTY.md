# Third-party code

| What | Where | License |
|---|---|---|
| [nanopb](https://github.com/nanopb/nanopb) 0.4.9.2 runtime | `src/hiflow_pb/nanopb/`, copied into `components/hiflow_ble/pb*` | zlib, `src/hiflow_pb/nanopb/LICENSE.txt` |
| [TheTiEr/hiflow-ble](https://github.com/TheTiEr/hiflow-ble) Python reference modules and `.proto` files | `test/ref/hiflow_ble/` (unmodified), `.proto` files adapted in `src/hiflow_pb/generated/` | MIT, `test/ref/hiflow_ble/LICENSE` |
| `generate_ble_id()` from TheTiEr/hiflow-ble | ported to `tools/gen_ble_id.py` | MIT |
| [esp_delta_ota](https://github.com/espressif/idf-extra-components/tree/master/esp_delta_ota) 1.1.4 with detools' C patch decoder | fetched by the ESP-IDF component manager at build time, used by `components/zigbee_ota/` | Apache-2.0 (detools part: BSD) |
| [detools](https://github.com/eerimoq/detools) Python package | installed by the user, used by `tools/make_zigbee_ota.py` for delta images | BSD |

The C protocol core in `src/hiflow_core/` is an independent C implementation of the protocol
that hiflow-ble documents. It is verified against vectors generated with the reference modules
(`test/gen_vectors.py`).
