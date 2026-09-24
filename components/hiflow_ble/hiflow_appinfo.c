/*
 * hiflow_appinfo.c — see hiflow_appinfo.h.
 *
 * The reply is decoded as APPInfoDataReqDTO: the vendor .proto names the two
 * directions the other way round (the reply to a "…ResDTO" request is a
 * "…ReqDTO"), exactly as with RealDataNew. Field 8 of that message is
 * APPDtuInfoMO, field 27 inside it is enc_rand.
 *
 * Only enc_rand needs static storage (APPInfomationData.options); every other
 * var-length field of the message is a pb_callback_t, which nanopb skips while
 * decoding when no callback is registered. That keeps the decode free of
 * max_count/max_size surprises and costs no heap.
 */
#include "hiflow_appinfo.h"

#include <string.h>

#include "pb_decode.h"
#include "APPInfomationData.pb.h"

int hiflow_appinfo_extract_enc_rand(const uint8_t *pt, size_t len,
                                    uint8_t out[HIFLOW_ENC_RAND_LEN],
                                    int64_t *device_time)
{
    /* Static, not on the stack: this runs from the GATTC notification
     * callback, and the decoded struct is a few hundred bytes. The component
     * is single-threaded (one BLE callback at a time), as is the host test. */
    static APPInfoDataReqDTO msg;
    pb_istream_t is;

    if (!pt || !out || len == 0)
        return HIFLOW_ERR_ARG;

    memset(&msg, 0, sizeof(msg));
    is = pb_istream_from_buffer(pt, len);
    if (!pb_decode(&is, APPInfoDataReqDTO_fields, &msg))
        return HIFLOW_ERR_FIELD; /* malformed -> PB_GET_ERROR(&is) */

    if (!msg.has_dtu_info)
        return HIFLOW_ERR_FIELD; /* no APPDtuInfoMO (field 8) in the reply */

    if (msg.dtu_info.enc_rand.size != HIFLOW_ENC_RAND_LEN)
        return HIFLOW_ERR_FIELD; /* no / wrong-sized encRand (field 27) */

    memcpy(out, msg.dtu_info.enc_rand.bytes, HIFLOW_ENC_RAND_LEN);
    if (device_time != NULL)
        *device_time = (int64_t) msg.timestamp;
    return HIFLOW_OK;
}
