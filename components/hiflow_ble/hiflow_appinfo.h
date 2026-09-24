/*
 * hiflow_appinfo.h — decodes the V0 pairing reply that carries encRand.
 *
 * The inverter's current V1 session key (encRand, 16 bytes) is only ever
 * obtainable through the V0 handshake: send an APP_INFO_DATA request
 * (cmd 0xA201/0xA301, SN-keyed AES-128-CBC, see hiflow_build_frame_v0) and the
 * reply holds APPDtuInfoMO.enc_rand (field 27) nested in the outer message's
 * field 8.
 *
 * Reference implementation (byte-for-byte source of truth):
 *   hiflow_ble/hiflow.py::_extract_enc_rand_from_appinfo in TheTiEr/hiflow-ble
 *   (not vendored under test/ref/)
 *   -> walks field 8, then field 27, and rejects anything that is not exactly
 *      16 bytes long.
 *
 * The protobuf decoder comes from src/hiflow_pb/generated/APPInfomationData.pb.*
 * (nanopb, generator 0.4.9.2, options in APPInfomationData.options).
 */
#ifndef HIFLOW_APPINFO_H
#define HIFLOW_APPINFO_H

#include <stddef.h>
#include <stdint.h>

#include "hiflow_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a *decrypted* APP_INFO_DATA payload (pt, len) and copy encRand into
 * `out` (HIFLOW_ENC_RAND_LEN bytes). When `device_time` is not NULL it receives
 * the reply's timestamp (field 2, the inverter's unix time), or 0 when the
 * reply has none.
 *
 * Returns
 *   HIFLOW_OK         on success,
 *   HIFLOW_ERR_ARG    for NULL/empty input,
 *   HIFLOW_ERR_FIELD  when the payload is malformed, has no dtu_info (field 8)
 *                     or no/properly-sized enc_rand (field 27).
 *
 * Not reentrant: the ~320 byte decode scratch buffer is static so that callers
 * on the BLE callback stack do not pay for it.
 */
int hiflow_appinfo_extract_enc_rand(const uint8_t *pt, size_t len,
                                    uint8_t out[HIFLOW_ENC_RAND_LEN],
                                    int64_t *device_time);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_APPINFO_H */
