/*
 * test_setup.c — host test for the bridge's own settings (hiflow_setup).
 *
 * Covers:
 *   - the text commands of the Zigbee settings attribute (pin=, sn=, forget,
 *     ant=), with the 15 characters the attribute holds
 *   - the inverter's BLE name and serial
 *   - the login identity the bridge makes up for itself
 *   - choosing the inverter: only one in range for the whole window, or the
 *     one named with sn=, never one of several by chance
 *
 * Build/run:  make -C test/host test-setup
 */
#include <stdio.h>
#include <string.h>

#include "hiflow_setup.h"

static int g_pass;
static int g_fail;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void parse_str(const char *text, hiflow_setup_cmd_t *cmd)
{
    hiflow_setup_parse(text, strlen(text), cmd);
}

static void test_commands(void)
{
    hiflow_setup_cmd_t c;

    printf("\n[1] commands\n");

    parse_str("", &c);
    check(c.type == HIFLOW_SETUP_CMD_NONE, "empty text: nothing to do");
    parse_str("   ", &c);
    check(c.type == HIFLOW_SETUP_CMD_NONE, "only spaces: nothing to do");
    hiflow_setup_parse(NULL, 5, &c);
    check(c.type == HIFLOW_SETUP_CMD_NONE, "no text at all: nothing to do");

    parse_str("pin=4711", &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && strcmp(c.value, "4711") == 0, "pin=4711");
    parse_str("  PIN=Ab12  ", &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && strcmp(c.value, "Ab12") == 0,
          "command word in any case, spaces around, value as written");
    parse_str("pin=", &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && c.value[0] == '\0', "pin= alone removes the PIN");
    parse_str("pin=12345678901", &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && strlen(c.value) == HIFLOW_SETUP_PIN_MAX,
          "the longest PIN that fits the 15 characters");
    parse_str("pin=123456789012", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD && strlen(c.error) <= HIFLOW_SETUP_TEXT_MAX,
          "a longer PIN is refused with a reply that fits");
    parse_str("pin=12 34", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "a space inside the PIN is refused");

    parse_str("sn=ABCdef123456", &c);
    check(c.type == HIFLOW_SETUP_CMD_SN && strcmp(c.value, "ABCdef123456") == 0, "sn= with 12 characters");
    parse_str("sn=ABCDEF12345", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "sn= with 11 characters is refused");
    parse_str("sn=ABCDEF-23456", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "sn= with a dash is refused");

    parse_str("forget", &c);
    check(c.type == HIFLOW_SETUP_CMD_FORGET, "forget");
    parse_str("Forget ", &c);
    check(c.type == HIFLOW_SETUP_CMD_FORGET, "Forget with a space after it");
    parse_str("forgetit", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "forgetit is not forget");

    parse_str("ant=ext", &c);
    check(c.type == HIFLOW_SETUP_CMD_ANTENNA && c.external == 1, "ant=ext");
    parse_str("ANT=INT", &c);
    check(c.type == HIFLOW_SETUP_CMD_ANTENNA && c.external == 0, "ANT=INT");
    parse_str("ant=external", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "ant=external is refused");

    parse_str("hello", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD && strlen(c.error) <= HIFLOW_SETUP_TEXT_MAX,
          "an unknown command gets a hint that fits");
}

static void test_names(void)
{
    char sn[HIFLOW_SN_LEN + 1];
    char id[HIFLOW_SETUP_BLE_ID_DIGITS + 1];
    uint8_t zeros[HIFLOW_SETUP_BLE_ID_DIGITS] = {0};
    uint8_t ff[HIFLOW_SETUP_BLE_ID_DIGITS];
    size_t i;

    printf("\n[2] names, serials and the login identity\n");

    check(hiflow_setup_name_serial("RMI-AB12cd34EF56", 16, sn) == 1 && strcmp(sn, "AB12cd34EF56") == 0,
          "RMI- name gives the serial as advertised");
    check(hiflow_setup_name_serial("RMI-AB12", 8, sn) == 0, "a short name is no inverter");
    check(hiflow_setup_name_serial("RMI-AB12cd34EF567", 17, sn) == 0, "a long name is no inverter");
    check(hiflow_setup_name_serial("XMI-AB12cd34EF56", 16, sn) == 0, "another prefix is no inverter");
    check(hiflow_setup_name_serial("RMI-AB12cd34EF5_", 16, sn) == 0, "a serial with _ is no inverter");
    check(hiflow_setup_name_serial(NULL, 16, sn) == 0, "no name");

    check(hiflow_setup_same_serial("AB12cd34EF56", "ab12CD34ef56") == 1, "serials compare without case");
    check(hiflow_setup_same_serial("AB12cd34EF56", "AB12cd34EF57") == 0, "different serials");
    check(hiflow_setup_same_serial("AB12", "AB12") == 0, "a short serial matches nothing");

    check(hiflow_setup_ble_id_valid("123456789012345678") == 1, "18 digits are an identity");
    check(hiflow_setup_ble_id_valid("") == 0, "empty is none");
    check(hiflow_setup_ble_id_valid("12345a") == 0, "letters are not");
    check(hiflow_setup_ble_id_valid("123456789012345678901234") == 0, "24 digits do not fit the session");

    hiflow_setup_make_ble_id(zeros, id);
    check(strcmp(id, "100000000000000000") == 0, "zero bytes: the first digit is still not 0");
    memset(ff, 0xFF, sizeof(ff));
    hiflow_setup_make_ble_id(ff, id);
    check(strcmp(id, "455555555555555555") == 0, "0xFF bytes: 255 % 9 and 255 % 10");
    for (i = 0; i < sizeof(ff); i++)
        ff[i] = (uint8_t) (i * 37 + 11);
    hiflow_setup_make_ble_id(ff, id);
    check(strlen(id) == HIFLOW_SETUP_BLE_ID_DIGITS && hiflow_setup_ble_id_valid(id) == 1,
          "a made-up identity is valid for the session");
}

#define MAC_A 0x111111111111ULL
#define MAC_B 0x222222222222ULL
#define SN_A "RMI-AAAAAA111111"
#define SN_B "RMI-BBBBBB222222"

static void test_discovery(void)
{
    hiflow_discovery_t d;
    uint64_t mac;
    char sn[HIFLOW_SN_LEN + 1];
    char name[17];
    int i;

    printf("\n[3] choosing the inverter\n");

    hiflow_discovery_start(&d, NULL);
    check(hiflow_discovery_check(&d, 0, &mac, sn) == HIFLOW_DISCOVERY_WAIT, "nothing seen: wait");
    check(hiflow_discovery_seen(&d, 1000, 0x333333333333ULL, "Phone", 5) == 0, "another device is ignored");
    check(hiflow_discovery_seen(&d, 1000, MAC_A, NULL, 0) == 0, "no name: not an inverter");
    check(hiflow_discovery_seen(&d, 1000, 0, SN_A, 16) == 0, "address 0 is ignored");

    check(hiflow_discovery_seen(&d, 1000, MAC_A, SN_A, 16) == 1, "an inverter");
    check(hiflow_discovery_seen(&d, 2000, MAC_A, SN_A, 16) == 1, "the same one again");
    check(d.count == 1, "counted once");
    check(hiflow_discovery_check(&d, 1000 + HIFLOW_DISCOVERY_WINDOW_MS - 1, &mac, sn) == HIFLOW_DISCOVERY_WAIT,
          "alone, but the window still runs");
    mac = 0;
    check(hiflow_discovery_check(&d, 1000 + HIFLOW_DISCOVERY_WINDOW_MS, &mac, sn) == HIFLOW_DISCOVERY_FOUND &&
              mac == MAC_A && strcmp(sn, "AAAAAA111111") == 0,
          "alone for the whole window: taken");

    hiflow_discovery_start(&d, "");
    hiflow_discovery_seen(&d, 1000, MAC_A, SN_A, 16);
    hiflow_discovery_seen(&d, 200000, MAC_B, SN_B, 16);
    check(hiflow_discovery_check(&d, 1000 + 2 * HIFLOW_DISCOVERY_WINDOW_MS, &mac, sn) == HIFLOW_DISCOVERY_SEVERAL,
          "two in range: neither is taken");

    hiflow_discovery_start(&d, "bbbbbb222222");
    hiflow_discovery_seen(&d, 1000, MAC_A, SN_A, 16);
    check(hiflow_discovery_check(&d, 1000 + 2 * HIFLOW_DISCOVERY_WINDOW_MS, &mac, sn) == HIFLOW_DISCOVERY_WAIT,
          "sn= given: a neighbour is never taken");
    hiflow_discovery_seen(&d, 3000, MAC_B, SN_B, 16);
    mac = 0;
    check(hiflow_discovery_check(&d, 3000, &mac, sn) == HIFLOW_DISCOVERY_FOUND && mac == MAC_B &&
              strcmp(sn, "BBBBBB222222") == 0,
          "sn= given: taken at once, serial as advertised");

    hiflow_discovery_start(&d, NULL);
    for (i = 0; i < HIFLOW_DISCOVERY_SLOTS + 1; i++) {
        snprintf(name, sizeof(name), "RMI-CCCCCC00000%d", i);
        hiflow_discovery_seen(&d, 1000, 0x500000000000ULL + (uint64_t) i, name, 16);
    }
    check(d.more == 1, "more inverters than slots");
    check(hiflow_discovery_check(&d, 1000 + 2 * HIFLOW_DISCOVERY_WINDOW_MS, &mac, sn) == HIFLOW_DISCOVERY_SEVERAL,
          "many in range: several");

    hiflow_discovery_start(&d, "DDDDDD999999");
    for (i = 0; i < HIFLOW_DISCOVERY_SLOTS + 2; i++) {
        snprintf(name, sizeof(name), "RMI-CCCCCC00000%d", i);
        hiflow_discovery_seen(&d, 1000, 0x500000000000ULL + (uint64_t) i, name, 16);
    }
    hiflow_discovery_seen(&d, 5000, 0x600000000000ULL, "RMI-DDDDDD999999", 16);
    mac = 0;
    check(hiflow_discovery_check(&d, 5000, &mac, sn) == HIFLOW_DISCOVERY_FOUND && mac == 0x600000000000ULL,
          "the one asked for is found even when its neighbours fill every slot");
}

int main(void)
{
    printf("hiflow_setup host tests\n");
    test_commands();
    test_names();
    test_discovery();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
