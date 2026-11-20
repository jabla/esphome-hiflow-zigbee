/*
 * test_setup.c — host test for the bridge's own settings (hiflow_setup).
 *
 * Covers:
 *   - the text commands over USB (pin=, sn=, forget, ant=, status)
 *   - the line reader in front of them, also with hostile input: overlong
 *     lines, binary garbage, partial lines, CR/LF variants
 *   - the inverter's BLE name and serial
 *   - the login identity the bridge makes up for itself
 *   - choosing the inverter: only one in range for the whole window, or the
 *     one named with sn=, never one of several by chance
 *   - the status line: its words, and that the longest one fits
 *
 * Build/run:  make -C test/host test-setup
 */
#include <stdio.h>
#include <stdlib.h>
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
    parse_str("pin=123456789012345", &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && strlen(c.value) == HIFLOW_SETUP_PIN_MAX &&
              strcmp(c.value, "123456789012345") == 0,
          "a PIN of the longest length");
    parse_str("pin=1234567890123456", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD && strstr(c.error, "1234") == NULL,
          "a longer PIN is refused, and the reason does not repeat it");
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

    parse_str("status", &c);
    check(c.type == HIFLOW_SETUP_CMD_STATUS, "status");
    parse_str(" STATUS\t", &c);
    check(c.type == HIFLOW_SETUP_CMD_STATUS, "STATUS with spaces around");
    parse_str("statusx", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD, "statusx is not status");

    parse_str("hello", &c);
    check(c.type == HIFLOW_SETUP_CMD_BAD && c.error != NULL && c.error[0] != '\0',
          "an unknown command gets a hint");
}

/* Feeds `n` bytes; counts what came out and keeps the last READY line. */
typedef struct {
    int ready;
    int too_long;
    char last[HIFLOW_SETUP_LINE_MAX + 1];
} feed_result_t;

static feed_result_t feed(hiflow_line_reader_t *r, const char *bytes, size_t n)
{
    feed_result_t out;
    size_t i;

    memset(&out, 0, sizeof(out));
    for (i = 0; i < n; i++) {
        switch (hiflow_line_put(r, (uint8_t) bytes[i])) {
            case HIFLOW_LINE_READY:
                out.ready++;
                memcpy(out.last, r->line, r->len + 1);
                break;
            case HIFLOW_LINE_TOO_LONG:
                out.too_long++;
                break;
            default:
                break;
        }
    }
    return out;
}

static feed_result_t feed_str(hiflow_line_reader_t *r, const char *s)
{
    return feed(r, s, strlen(s));
}

static int printable(const char *s)
{
    for (; *s != '\0'; s++) {
        if (((unsigned char) *s < 0x20 && *s != '\t') || (unsigned char) *s > 0x7E)
            return 0;
    }
    return 1;
}

static void test_lines(void)
{
    hiflow_line_reader_t r;
    feed_result_t f;
    char buf[2048];
    hiflow_setup_cmd_t c;
    int i;

    printf("\n[2] lines from the USB port\n");

    hiflow_line_reader_init(&r);
    f = feed_str(&r, "status\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "a line ending in LF");
    f = feed_str(&r, "status\r");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "a line ending in CR");
    f = feed_str(&r, "status\r\n");
    check(f.ready == 1, "CR LF is one line, the empty one after it is dropped");
    f = feed_str(&r, "status\n\r");
    check(f.ready == 1, "LF CR is one line too");
    f = feed_str(&r, "\n\r\n\r\r\n");
    check(f.ready == 0 && f.too_long == 0, "empty lines are dropped without an answer");
    f = feed_str(&r, "pin=1\nsn=AAAAAA111111\r\nstatus\n");
    check(f.ready == 3 && strcmp(f.last, "status") == 0, "three lines in one go");

    f = feed_str(&r, "sta");
    check(f.ready == 0, "a partial line waits");
    f = feed_str(&r, "tus\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "and is complete with the rest");

    memset(buf, 'a', HIFLOW_SETUP_LINE_MAX);
    buf[HIFLOW_SETUP_LINE_MAX] = '\n';
    f = feed(&r, buf, HIFLOW_SETUP_LINE_MAX + 1);
    check(f.ready == 1 && strlen(f.last) == HIFLOW_SETUP_LINE_MAX, "a line of the longest length");
    memset(buf, 'a', HIFLOW_SETUP_LINE_MAX + 1);
    buf[HIFLOW_SETUP_LINE_MAX + 1] = '\n';
    f = feed(&r, buf, HIFLOW_SETUP_LINE_MAX + 2);
    check(f.ready == 0 && f.too_long == 1, "one byte more: too long");
    memset(buf, 'p', sizeof(buf));
    buf[sizeof(buf) - 1] = '\n';
    f = feed(&r, buf, sizeof(buf));
    check(f.ready == 0 && f.too_long == 1, "a very long line: too long, once");
    f = feed_str(&r, "status\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "the next line is read again");

    f = feed_str(&r, "pin=123456789012345\n");
    check(f.ready == 1, "the longest PIN fits a line");
    hiflow_setup_parse(f.last, strlen(f.last), &c);
    check(c.type == HIFLOW_SETUP_CMD_PIN && strcmp(c.value, "123456789012345") == 0, "and is read whole");
    f = feed_str(&r, "pin=\r\n");
    hiflow_setup_parse(f.last, strlen(f.last), &c);
    check(f.ready == 1 && c.type == HIFLOW_SETUP_CMD_PIN && c.value[0] == '\0', "an empty PIN removes it");

    /* ESP Web Tools' Improv request: "IMPROV", version 1, RPC 3, length 2,
       command 2, no data, checksum, LF. */
    f = feed(&r, "IMPROV\x01\x03\x02\x02\x00\xe6\n", 12);
    check(f.ready == 0 && f.too_long == 0, "an Improv frame is dropped without an answer");
    f = feed(&r, "stat\0us\n", 8);
    check(f.ready == 0, "a line with a NUL is dropped");
    f = feed_str(&r, "pin=12\x7f" "34\n");
    check(f.ready == 1 && strcmp(f.last, "pin=134") == 0, "DEL takes back the byte before it");
    f = feed_str(&r, "statsu\b\bus\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "so does Backspace");
    f = feed_str(&r, "\b\x7fstatus\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "Backspace on an empty line does nothing");
    f = feed_str(&r, "ab\b\b\n");
    check(f.ready == 0 && f.too_long == 0, "a line taken back to nothing is empty");
    f = feed_str(&r, "\x01status\b\n");
    check(f.ready == 0, "Backspace keeps a binary line dropped");
    memset(buf, 'x', 40);
    buf[40] = '\b';
    buf[41] = '\n';
    f = feed(&r, buf, 42);
    check(f.ready == 0 && f.too_long == 1, "Backspace after too many bytes keeps it too long");
    hiflow_line_reader_init(&r);
    f = feed_str(&r, "pin=4711\b\b\b\b");
    for (i = (int) r.len; i < (int) sizeof(r.line); i++)
        if (r.line[i] != '\0')
            break;
    check(r.len == 4 && i == (int) sizeof(r.line), "a byte taken back is wiped (it may be part of a PIN)");
    hiflow_line_reader_init(&r);
    f = feed_str(&r, "pin=12\xc3\xa4" "34\n");
    check(f.ready == 0, "a line with UTF-8 is dropped");
    f = feed_str(&r, "\x1b[0;33mstatus\n");
    check(f.ready == 0, "a line with an escape sequence is dropped");
    f = feed_str(&r, "status\n");
    check(f.ready == 1, "a line after the garbage is read");

    f = feed_str(&r, "pin=12\x15status\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "Ctrl-U drops the line so far");
    f = feed_str(&r, "\x01\x02\x15status\n");
    check(f.ready == 1 && strcmp(f.last, "status") == 0, "also after binary bytes");
    memset(buf, 'x', 100);
    buf[100] = 0x15;
    memcpy(buf + 101, "status\n", 7);
    f = feed(&r, buf, 108);
    check(f.ready == 1 && f.too_long == 0, "also after too many bytes");

    f = feed_str(&r, "pin=4711\n");
    check(f.ready == 1, "a PIN line");
    hiflow_line_done(&r);
    check(r.len == 0 && strstr(r.line, "4711") == NULL && r.line[0] == '\0', "done clears it");
    f = feed_str(&r, "pin=4711\n");
    hiflow_line_put(&r, 's');
    check(strstr(r.line, "4711") == NULL, "so does the next byte");
    hiflow_line_reader_init(&r);

    srand(12345);
    for (i = 0; i < 200000; i++) {
        uint8_t b = (uint8_t) (rand() & 0xFF);
        if ((rand() & 7) == 0)
            b = (uint8_t) "\r\n\x15 =a"[rand() % 6];
        if (hiflow_line_put(&r, b) == HIFLOW_LINE_READY) {
            if (r.len > HIFLOW_SETUP_LINE_MAX || strlen(r.line) != r.len || !printable(r.line)) {
                check(0, "random bytes: a READY line is printable and fits");
                break;
            }
            hiflow_setup_parse(r.line, r.len, &c);
        }
    }
    check(i == 200000, "random bytes: every READY line is printable and fits");
}

static void test_names(void)
{
    char sn[HIFLOW_SN_LEN + 1];
    char id[HIFLOW_SETUP_BLE_ID_DIGITS + 1];
    uint8_t zeros[HIFLOW_SETUP_BLE_ID_DIGITS] = {0};
    uint8_t ff[HIFLOW_SETUP_BLE_ID_DIGITS];
    size_t i;

    printf("\n[3] names, serials and the login identity\n");

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

    printf("\n[4] choosing the inverter\n");

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

static void test_status(void)
{
    hiflow_setup_status_t st;
    hiflow_discovery_t d;
    char out[HIFLOW_SETUP_STATUS_MAX];
    char small[20];
    size_t n;
    int i;

    printf("\n[5] the status line\n");

    memset(&st, 0, sizeof(st));
    st.sn = "AB12cd34EF56";
    st.pin = HIFLOW_SETUP_PIN_SET;
    st.session = 6;
    st.antenna = 1;
    n = hiflow_setup_format_status(out, sizeof(out), &st);
    check(strcmp(out, "status sn=AB12cd34EF56 pin=set session=6 antenna=ext") == 0 && n == strlen(out),
          "an inverter chosen");

    hiflow_discovery_start(&d, NULL);
    st.sn = NULL;
    st.discovery = &d;
    st.pin = HIFLOW_SETUP_PIN_NONE;
    st.session = 1;
    st.antenna = -1;
    hiflow_setup_format_status(out, sizeof(out), &st);
    check(strcmp(out, "status sn=- found=0 pin=none session=1") == 0, "searching, none in range");

    hiflow_discovery_seen(&d, 1000, MAC_A, SN_A, 16);
    st.now_ms = 61000;
    st.antenna = 0;
    hiflow_setup_format_status(out, sizeof(out), &st);
    check(strcmp(out, "status sn=- found=1 seen=AAAAAA111111 take_in=240 pin=none session=1 antenna=int") == 0,
          "one in range: when it is taken");
    st.now_ms = 1000 + 2 * HIFLOW_DISCOVERY_WINDOW_MS;
    hiflow_setup_format_status(out, sizeof(out), &st);
    check(strstr(out, " take_in=0 ") != NULL, "the time left does not go below 0");

    hiflow_discovery_seen(&d, 2000, MAC_B, SN_B, 16);
    st.session = 16;
    st.antenna = -1;
    st.pin = HIFLOW_SETUP_PIN_BUILD;
    hiflow_setup_format_status(out, sizeof(out), &st);
    check(strcmp(out, "status sn=- found=2 seen=AAAAAA111111,BBBBBB222222 pin=build session=16") == 0,
          "two in range: both serials");

    hiflow_discovery_start(&d, "bbbbbb222222");
    hiflow_discovery_seen(&d, 1000, MAC_A, SN_A, 16);
    st.session = 1;
    hiflow_setup_format_status(out, sizeof(out), &st);
    check(strcmp(out, "status sn=- want=bbbbbb222222 found=1 seen=AAAAAA111111 pin=build session=1") == 0,
          "looking for the one given: no time to take another");

    /* The longest line: a serial asked for, every slot full and more. */
    hiflow_discovery_start(&d, "DDDDDD999999");
    for (i = 0; i < HIFLOW_DISCOVERY_SLOTS + 1; i++) {
        char name[24];
        snprintf(name, sizeof(name), "RMI-CCCCCC00000%d", i);
        hiflow_discovery_seen(&d, 1000, 0x500000000000ULL + (uint64_t) i, name, 16);
    }
    st.session = -2147483647 - 1;
    st.antenna = 1;
    n = hiflow_setup_format_status(out, sizeof(out), &st);
    check(strstr(out, " found=4+ seen=CCCCCC000000,CCCCCC000001,CCCCCC000002,CCCCCC000003 ") != NULL,
          "more than the slots: 4+, the four kept");
    check(n == strlen(out) && n + 1 < sizeof(out) && strstr(out, " antenna=ext") != NULL,
          "the longest line fits HIFLOW_SETUP_STATUS_MAX");

    memset(small, 'Z', sizeof(small));
    n = hiflow_setup_format_status(small, 12, &st);
    check(n == 11 && small[11] == '\0' && small[12] == 'Z' && strncmp(small, "status sn=-", 11) == 0,
          "a small buffer: cut, terminated, nothing written past it");
    check(hiflow_setup_format_status(small, 0, &st) == 0 && small[0] == 's', "size 0 writes nothing");
}

int main(void)
{
    printf("hiflow_setup host tests\n");
    test_commands();
    test_lines();
    test_names();
    test_discovery();
    test_status();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
