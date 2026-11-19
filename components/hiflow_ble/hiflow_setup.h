/*
 * hiflow_setup — what the bridge needs to know about its inverter, without
 * any of it in the firmware image.
 *
 * A release image is the same for every bridge of a board, so it carries no
 * personal data. The bridge finds its inverter by the BLE name ("RMI-" and the
 * 12-character serial, which the V0 pairing needs) and makes up its own login
 * identity. The rest comes as short text commands through one Zigbee attribute,
 * the Basic cluster's LocationDescription (15 characters, readable and writable
 * in ZHA's "Manage Zigbee device" dialog and in Zigbee2MQTT's dev console):
 *
 *   pin=123456   the BLE PIN from the S-Miles app; "pin=" alone removes it
 *   sn=ABCDEF123456  the inverter by its serial, when several are in range
 *   forget       drop the inverter and look for one again
 *   ant=ext      the external antenna (XIAO ESP32-C6), ant=int the internal one
 *
 * This file holds the parts without ESPHome or BLE APIs, so the host tests
 * cover them: reading a command, reading a BLE name, choosing the inverter.
 * The component keeps the values in flash and answers in the same attribute.
 */
#ifndef HIFLOW_SETUP_H
#define HIFLOW_SETUP_H

#include <stddef.h>
#include <stdint.h>

#include "hiflow_frame.h" /* HIFLOW_SN_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* The bridge takes over the LocationDescription that ESPHome creates from the
   area text, which it cuts to 15 characters (the ZCL spec allows 16). The
   Zigbee stack keeps the length of that first text. */
#define HIFLOW_SETUP_TEXT_MAX 15

/* The longest PIN that fits behind "pin=". */
#define HIFLOW_SETUP_PIN_MAX (HIFLOW_SETUP_TEXT_MAX - 4)

/* The login identity the bridge makes up for itself: 18 decimal digits, like
   the S-Miles app's (tools/gen_ble_id.py). */
#define HIFLOW_SETUP_BLE_ID_DIGITS 18

/* The longest identity the session takes: its buffer (HIFLOW_BLE_ID_LEN in
   hiflow_session.h) holds 23 characters and the NUL. */
#define HIFLOW_SETUP_BLE_ID_MAX 23

typedef enum {
    HIFLOW_SETUP_CMD_NONE = 0, /* empty text: nothing to do */
    HIFLOW_SETUP_CMD_PIN,      /* value: the PIN, "" removes it */
    HIFLOW_SETUP_CMD_SN,       /* value: the serial as typed */
    HIFLOW_SETUP_CMD_FORGET,
    HIFLOW_SETUP_CMD_ANTENNA,  /* external: 1 or 0 */
    HIFLOW_SETUP_CMD_BAD       /* error: a short reason, fits the attribute */
} hiflow_setup_cmd_type_t;

typedef struct {
    hiflow_setup_cmd_type_t type;
    char value[HIFLOW_SETUP_TEXT_MAX + 1];
    int external;
    const char *error;
} hiflow_setup_cmd_t;

/* Reads a command from the attribute's text (`len` bytes, not NUL-terminated).
   Spaces around it do not count, the command word is not case-sensitive, the
   value is taken as written. */
void hiflow_setup_parse(const char *text, size_t len, hiflow_setup_cmd_t *cmd);

/* 1 if `name` (`len` bytes) is an inverter's BLE name, "RMI-" and the 12
   characters of its serial; `sn` then holds the serial as advertised. */
int hiflow_setup_name_serial(const char *name, size_t len, char sn[HIFLOW_SN_LEN + 1]);

/* 1 if two serials are the same, ignoring case. */
int hiflow_setup_same_serial(const char *a, const char *b);

/* 1 for a login identity the session can send: 1 to HIFLOW_SETUP_BLE_ID_MAX
   decimal digits. */
int hiflow_setup_ble_id_valid(const char *ble_id);

/* A new login identity from 18 random bytes: 18 digits, the first not 0. */
void hiflow_setup_make_ble_id(const uint8_t random[HIFLOW_SETUP_BLE_ID_DIGITS],
                              char out[HIFLOW_SETUP_BLE_ID_DIGITS + 1]);

/* ---------- choosing the inverter ---------- */

/* Distinct inverters remembered while looking; more only count. */
#define HIFLOW_DISCOVERY_SLOTS 4

/* Without a serial given, an inverter is taken only once it has been the only
   one in range for this long after it was first seen. A second one in that
   time means the owner has to name theirs (sn=). */
#define HIFLOW_DISCOVERY_WINDOW_MS ((int64_t) 5 * 60 * 1000)

typedef enum {
    HIFLOW_DISCOVERY_WAIT = 0, /* none yet, or the window still runs */
    HIFLOW_DISCOVERY_FOUND,    /* mac and sn are set */
    HIFLOW_DISCOVERY_SEVERAL   /* more than one in range and no serial given */
} hiflow_discovery_result_t;

typedef struct {
    char     want[HIFLOW_SN_LEN + 1]; /* the serial given with sn=, "" = any */
    int      count;                   /* distinct inverters kept below */
    int      more;                    /* 1 once another one did not fit */
    uint64_t mac[HIFLOW_DISCOVERY_SLOTS];
    char     sn[HIFLOW_DISCOVERY_SLOTS][HIFLOW_SN_LEN + 1];
    int64_t  first_ms;                /* first sighting, -1 before */
} hiflow_discovery_t;

/* Starts looking, for the inverter with serial `want` (NULL or "" for any). */
void hiflow_discovery_start(hiflow_discovery_t *d, const char *want);

/* One advertisement: its address and its name, if it had one. Returns 1 if it
   was an inverter. */
int hiflow_discovery_seen(hiflow_discovery_t *d, int64_t now_ms, uint64_t mac,
                          const char *name, size_t name_len);

/* Where the search stands. FOUND sets `mac` and `sn` (as advertised). */
hiflow_discovery_result_t hiflow_discovery_check(const hiflow_discovery_t *d, int64_t now_ms,
                                                 uint64_t *mac, char sn[HIFLOW_SN_LEN + 1]);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_SETUP_H */
