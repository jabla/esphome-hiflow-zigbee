/*
 * hiflow_setup — commands, BLE names and the choice of the inverter.
 * See hiflow_setup.h.
 */
#include "hiflow_setup.h"

#include <string.h>

static const char NAME_PREFIX[] = "RMI-";
#define NAME_PREFIX_LEN (sizeof(NAME_PREFIX) - 1)

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/* 1 if text[0..len) starts with `word` (lower case), ignoring case. */
static int starts_with(const char *text, size_t len, const char *word)
{
    size_t n = strlen(word);
    size_t i;

    if (len < n)
        return 0;
    for (i = 0; i < n; i++) {
        if (lower(text[i]) != word[i])
            return 0;
    }
    return 1;
}

static int all_alnum(const char *s, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (!is_alnum(s[i]))
            return 0;
    }
    return 1;
}

static void bad(hiflow_setup_cmd_t *cmd, const char *why)
{
    cmd->type = HIFLOW_SETUP_CMD_BAD;
    cmd->error = why;
}

void hiflow_setup_parse(const char *text, size_t len, hiflow_setup_cmd_t *cmd)
{
    const char *value;
    size_t value_len;

    memset(cmd, 0, sizeof(*cmd));
    if (text == NULL)
        len = 0;
    while (len > 0 && is_space(text[0])) {
        text++;
        len--;
    }
    while (len > 0 && is_space(text[len - 1]))
        len--;
    if (len == 0) {
        cmd->type = HIFLOW_SETUP_CMD_NONE;
        return;
    }

    if (len == 6 && starts_with(text, len, "forget")) {
        cmd->type = HIFLOW_SETUP_CMD_FORGET;
        return;
    }

    if (starts_with(text, len, "pin=")) {
        value = text + 4;
        value_len = len - 4;
        if (value_len > HIFLOW_SETUP_PIN_MAX) {
            bad(cmd, "pin too long");
            return;
        }
        if (!all_alnum(value, value_len)) {
            bad(cmd, "pin: 0-9 a-z");
            return;
        }
        cmd->type = HIFLOW_SETUP_CMD_PIN;
        memcpy(cmd->value, value, value_len);
        cmd->value[value_len] = '\0';
        return;
    }

    if (starts_with(text, len, "sn=")) {
        value = text + 3;
        value_len = len - 3;
        if (value_len != HIFLOW_SN_LEN || !all_alnum(value, value_len)) {
            bad(cmd, "sn: 12 chars");
            return;
        }
        cmd->type = HIFLOW_SETUP_CMD_SN;
        memcpy(cmd->value, value, value_len);
        cmd->value[value_len] = '\0';
        return;
    }

    if (starts_with(text, len, "ant=")) {
        if (len == 7 && starts_with(text + 4, 3, "ext")) {
            cmd->type = HIFLOW_SETUP_CMD_ANTENNA;
            cmd->external = 1;
            return;
        }
        if (len == 7 && starts_with(text + 4, 3, "int")) {
            cmd->type = HIFLOW_SETUP_CMD_ANTENNA;
            cmd->external = 0;
            return;
        }
        bad(cmd, "ant=ext or int");
        return;
    }

    bad(cmd, "? pin= sn= ant=");
}

int hiflow_setup_name_serial(const char *name, size_t len, char sn[HIFLOW_SN_LEN + 1])
{
    if (name == NULL || len != NAME_PREFIX_LEN + HIFLOW_SN_LEN)
        return 0;
    if (memcmp(name, NAME_PREFIX, NAME_PREFIX_LEN) != 0)
        return 0;
    if (!all_alnum(name + NAME_PREFIX_LEN, HIFLOW_SN_LEN))
        return 0;
    memcpy(sn, name + NAME_PREFIX_LEN, HIFLOW_SN_LEN);
    sn[HIFLOW_SN_LEN] = '\0';
    return 1;
}

int hiflow_setup_same_serial(const char *a, const char *b)
{
    size_t i;

    if (a == NULL || b == NULL || strlen(a) != HIFLOW_SN_LEN || strlen(b) != HIFLOW_SN_LEN)
        return 0;
    for (i = 0; i < HIFLOW_SN_LEN; i++) {
        if (lower(a[i]) != lower(b[i]))
            return 0;
    }
    return 1;
}

int hiflow_setup_ble_id_valid(const char *ble_id)
{
    size_t i, n;

    if (ble_id == NULL)
        return 0;
    n = strlen(ble_id);
    if (n == 0 || n > HIFLOW_SETUP_BLE_ID_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        if (ble_id[i] < '0' || ble_id[i] > '9')
            return 0;
    }
    return 1;
}

void hiflow_setup_make_ble_id(const uint8_t random[HIFLOW_SETUP_BLE_ID_DIGITS],
                              char out[HIFLOW_SETUP_BLE_ID_DIGITS + 1])
{
    int i;

    out[0] = (char) ('1' + random[0] % 9);
    for (i = 1; i < HIFLOW_SETUP_BLE_ID_DIGITS; i++)
        out[i] = (char) ('0' + random[i] % 10);
    out[HIFLOW_SETUP_BLE_ID_DIGITS] = '\0';
}

/* ---------- choosing the inverter ---------- */

void hiflow_discovery_start(hiflow_discovery_t *d, const char *want)
{
    memset(d, 0, sizeof(*d));
    d->first_ms = -1;
    if (want != NULL && strlen(want) == HIFLOW_SN_LEN)
        memcpy(d->want, want, HIFLOW_SN_LEN + 1);
}

int hiflow_discovery_seen(hiflow_discovery_t *d, int64_t now_ms, uint64_t mac,
                          const char *name, size_t name_len)
{
    char sn[HIFLOW_SN_LEN + 1];
    int i, slot;

    if (mac == 0 || !hiflow_setup_name_serial(name, name_len, sn))
        return 0;
    for (i = 0; i < d->count; i++) {
        if (d->mac[i] == mac)
            return 1;
    }
    slot = d->count;
    if (slot == HIFLOW_DISCOVERY_SLOTS) {
        /* Full. Only the inverter asked for still gets in, in the last slot:
           it must not be crowded out by its neighbours. */
        d->more = 1;
        if (d->want[0] == '\0' || !hiflow_setup_same_serial(sn, d->want))
            return 1;
        slot = HIFLOW_DISCOVERY_SLOTS - 1;
    } else {
        d->count++;
    }
    d->mac[slot] = mac;
    memcpy(d->sn[slot], sn, sizeof(sn));
    if (d->first_ms < 0)
        d->first_ms = now_ms;
    return 1;
}

hiflow_discovery_result_t hiflow_discovery_check(const hiflow_discovery_t *d, int64_t now_ms,
                                                 uint64_t *mac, char sn[HIFLOW_SN_LEN + 1])
{
    int i;

    if (d->want[0] != '\0') {
        for (i = 0; i < d->count; i++) {
            if (hiflow_setup_same_serial(d->sn[i], d->want)) {
                *mac = d->mac[i];
                memcpy(sn, d->sn[i], HIFLOW_SN_LEN + 1);
                return HIFLOW_DISCOVERY_FOUND;
            }
        }
        return HIFLOW_DISCOVERY_WAIT;
    }
    if (d->count == 0)
        return HIFLOW_DISCOVERY_WAIT;
    if (d->count > 1 || d->more)
        return HIFLOW_DISCOVERY_SEVERAL;
    if (now_ms - d->first_ms < HIFLOW_DISCOVERY_WINDOW_MS)
        return HIFLOW_DISCOVERY_WAIT;
    *mac = d->mac[0];
    memcpy(sn, d->sn[0], HIFLOW_SN_LEN + 1);
    return HIFLOW_DISCOVERY_FOUND;
}
