/*
 * test_ota_blocks.c — host test for the OTA block bookkeeping
 * (components/zigbee_ota/ota_blocks.c).
 *
 *   [1] single answers: in order, repeat, gap, partial overlap, the end
 *   [2] a recorded gap from a download through Zigbee2MQTT, step by step
 *   [2b] three requests in flight: one move of FileOffset, not one per gap
 *   [3] whole downloads against a model of the stack and two servers:
 *       "zha" answers every request, "z2m" takes one request at a time and
 *       drops what comes while it answers. Some answers come seconds late
 *       (after the stack's retry), some requests get lost. The stress
 *       setting asks again after 300 ms, so many requests are in flight.
 *
 * Build/run:  make -C test/host test-ota   (-v on the binary: per run results)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_blocks.h"

static int g_pass;
static int g_fail;
static int g_verbose;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %u want %u\n", what, (unsigned) got, (unsigned) want);
    }
}

/* The stack's side of one answer: FileOffset grows by the answer's length
 * before the callback; then the callback's step applies. */
static ota_block_step_t answer(ota_blocks_t *b, uint32_t *fo, uint32_t offset, uint32_t len)
{
    ota_block_step_t s;

    *fo += len;
    s = ota_blocks_answer(b, offset, len, *fo);
    if (s.set_offset)
        *fo = s.offset;
    return s;
}

static void test_single(void)
{
    ota_blocks_t b;
    ota_block_step_t s;
    uint32_t fo = 0;

    printf("\n[1] single answers\n");

    ota_blocks_start(&b, 1000, 50);
    s = answer(&b, &fo, 0, 50);
    check(s.skip == 0 && s.take == 50 && !s.set_offset, "first block taken");
    s = answer(&b, &fo, 50, 50);
    check_u32(b.written, 100, "second block taken");
    check_u32(fo, 100, "FileOffset untouched mid-file");

    /* Repeat of block 50: the stack counted it, asks for 150 while the request
     * for 100 is still out. FileOffset stays ahead. */
    s = answer(&b, &fo, 50, 50);
    check(s.take == 0 && !s.gap && !s.set_offset, "repeat skipped, FileOffset stays ahead");
    check_u32(fo, 150, "stack one block ahead");
    s = answer(&b, &fo, 100, 50);
    check(s.take == 50 && !s.set_offset, "pipelined answer taken");
    check_u32(fo, 200, "stack still one block ahead");

    /* Gap: 200 arrives while 150 is missing. The request for 250 is out;
     * FileOffset goes a block behind the data. */
    s = answer(&b, &fo, 200, 50);
    check(s.take == 0 && s.gap && s.set_offset, "gap skipped");
    check_u32(fo, 100, "FileOffset a block behind the data");
    check_u32(b.misses, 1, "one miss counted");
    s = answer(&b, &fo, 250, 50);
    check(s.take == 0 && s.gap && !s.set_offset, "second gap: the stack already asks for the data's end");
    check_u32(fo, 150, "stack asks for 150");
    s = answer(&b, &fo, 150, 50);
    check(s.take == 50, "missing block taken");
    check_u32(b.misses, 0, "misses reset");

    /* Partial overlap: an answer from 180 brings 20 old and 30 new bytes. */
    s = answer(&b, &fo, 180, 50);
    check(s.skip == 20 && s.take == 30, "partial overlap: the new tail taken");
    check_u32(b.written, 230, "written up to 230");

    /* Too many answers out of place in a row. */
    ota_blocks_start(&b, 100000, 50);
    fo = 0;
    answer(&b, &fo, 0, 50);
    for (int i = 0; i < OTA_BLOCKS_MAX_MISSES; i++)
        s = answer(&b, &fo, 0, 50);
    check(!s.give_up, "OTA_BLOCKS_MAX_MISSES repeats tolerated");
    s = answer(&b, &fo, 0, 50);
    check(s.give_up, "one more gives up");
}

static void test_end(void)
{
    ota_blocks_t b;
    ota_block_step_t s;
    uint32_t fo;

    printf("[1b] the end of the file (1020 bytes, last block 20)\n");

    /* In order: the request for the last block is out when the block before it
     * is taken. FileOffset goes a block behind. The last block completes the
     * data; the answer still on its way ends the download. */
    ota_blocks_start(&b, 1020, 50);
    fo = 0;
    for (uint32_t off = 0; off < 950; off += 50)
        answer(&b, &fo, off, 50);
    check_u32(fo, 950, "before the end: FileOffset at the data");
    s = answer(&b, &fo, 950, 50);
    check(s.set_offset, "a full answer could end it now: FileOffset back");
    check_u32(fo, 950, "a block behind the data (1000)");
    s = answer(&b, &fo, 1000, 20);
    check_u32(b.written, 1020, "last block taken");
    check_u32(fo, 1019, "complete: FileOffset one byte short of the end");
    s = answer(&b, &fo, 970, 50); /* the answer to the request for 970 */
    check(fo >= 1020 && s.take == 0 && s.explained && !s.set_offset,
          "the answer still on its way ends the download, its skip expected");

    /* Only the last block missing, then a late repeat of the block before it.
     * With FileOffset at the data (1000) the stack would count it to 1050. It
     * would end the download 20 bytes short before the callback runs. */
    ota_blocks_start(&b, 1020, 50);
    fo = 0;
    for (uint32_t off = 0; off < 1000; off += 50)
        answer(&b, &fo, off, 50);
    check_u32(b.written, 1000, "all but the last block");
    s = answer(&b, &fo, 950, 50);
    check(s.take == 0 && fo < 1020, "late repeat before the last block: no early end");
    check_u32(fo, 950, "FileOffset still a block behind");
    s = answer(&b, &fo, 1000, 20);
    check_u32(b.written, 1020, "last block taken after the repeat");
    check_u32(fo, 1019, "then any answer ends the download");

    /* Two requests out a few blocks before the end (a late repeat of 850 while
     * 900 is out): no answer ends the download before the data is complete. */
    ota_blocks_start(&b, 1020, 50);
    fo = 0;
    for (uint32_t off = 0; off < 900; off += 50)
        answer(&b, &fo, off, 50);
    answer(&b, &fo, 850, 50);
    check_u32(fo, 950, "repeat: the stack a block ahead (900 and 950 out)");
    answer(&b, &fo, 900, 50);
    check_u32(fo, 900, "near the end: FileOffset a block behind (950, 1000 out)");
    answer(&b, &fo, 950, 50);
    check(b.written == 1000 && fo < 1020, "no early end (1000, 950 out)");
    answer(&b, &fo, 1000, 20);
    check(b.written == 1020 && fo == 1019, "complete (950, 970 out)");
    s = answer(&b, &fo, 950, 50);
    check(fo >= 1020 && s.explained && !s.set_offset, "the repeat of 950 ends it");

    /* The server's first answers are short, a later one is long (a server may
     * send less than the MaximumDataSize asked for). The guard near the end
     * uses that maximum, not the longest answer seen so far. */
    ota_blocks_start(&b, 120, 60);
    fo = 0;
    for (uint32_t off = 0; off < 60; off += 10)
        answer(&b, &fo, off, 10);
    check_u32(fo, 50, "short answers: FileOffset behind the data before a 60 byte answer could end it");
    s = answer(&b, &fo, 0, 60);
    check(s.take == 0 && fo < 120, "a long late repeat does not end the download");
    check_u32(b.written, 60, "data unchanged");
}

static void test_recorded(void)
{
    ota_blocks_t b;
    ota_block_step_t s;
    uint32_t fo;

    printf("[2] recorded gap (Zigbee2MQTT, 50 byte blocks, 356592 bytes)\n");

    /* Data up to 68500, stack asked for 68500. An answer for 67700 comes 16
     * blocks late: the stack counts it and asks for 68550. Zigbee2MQTT then
     * answers 68550 and never 68500. */
    ota_blocks_start(&b, 356592, 50);
    fo = 0;
    for (uint32_t off = 0; off < 68500; off += 50)
        answer(&b, &fo, off, 50);
    check_u32(fo, 68500, "in step before the late answer");
    s = answer(&b, &fo, 67700, 50);
    check(s.take == 0 && !s.gap && !s.set_offset, "late repeat skipped");
    s = answer(&b, &fo, 68550, 50);
    check(s.take == 0 && s.gap && s.set_offset, "gap skipped, FileOffset back");
    check_u32(fo, 68450, "a block behind the data");
    s = answer(&b, &fo, 68600, 50); /* the request sent with the gap */
    check(s.take == 0 && !s.set_offset, "its answer skipped too");
    check_u32(fo, 68500, "stack asks for the missing block");
    s = answer(&b, &fo, 68500, 50);
    check(s.take == 50, "missing block taken");
    check_u32(fo, 68550, "back in step");
}

static void test_in_flight(void)
{
    ota_blocks_t b;
    ota_block_step_t s;
    uint32_t fo = 0;

    printf("[2b] three requests in flight, the one for the data's end lost\n");

    /* Two late repeats leave requests for 1000, 1050 and 1100 out. */
    ota_blocks_start(&b, 100000, 50);
    for (uint32_t off = 0; off < 1000; off += 50)
        answer(&b, &fo, off, 50);
    answer(&b, &fo, 900, 50);
    answer(&b, &fo, 900, 50);
    check_u32(fo, 1100, "requests for 1000, 1050, 1100 out");

    /* 1050 answers before 1000: the request for 1000 was overtaken. */
    s = answer(&b, &fo, 1050, 50);
    check(s.gap && s.set_offset, "overtaken: FileOffset back");
    check_u32(fo, 950, "a block behind the data");
    /* The answer to 1100 brings the new request for 1000; the answer to
     * 1150 (sent before it) must not move FileOffset again. */
    s = answer(&b, &fo, 1100, 50);
    check(s.gap && !s.set_offset, "the new request for 1000 is out");
    s = answer(&b, &fo, 1150, 50);
    check(s.gap && !s.set_offset, "an older request's answer: no second move");
    check_u32(b.misses, 3, "misses: the two repeats and the first gap, not the older answers");
    check_u32(fo, 1050, "the stack asks on from the data's end");
    s = answer(&b, &fo, 1000, 50);
    check(s.take == 50, "missing block taken");
    s = answer(&b, &fo, 1050, 50);
    check(s.take == 50, "and the next one");
}

/* ---- whole downloads against a model ---------------------------------- */

#define Z2M_PAUSE_MS 250
#define DEV_MAX_LEN 223 /* zigbee_ota's default block_size: the MaximumDataSize asked for */

typedef struct {
    const char *name;
    uint32_t retry_ms, retries; /* the stack's block retry */
    uint32_t lo_ms, hi_ms;      /* time for an answer, request to answer */
    uint32_t slow_pm;           /* answers that take 300 to 900 ms */
    uint32_t late_pm;           /* answers that come 10.5 to 14 s late */
    uint32_t lost_pm;           /* requests that never arrive */
    uint32_t short_pm;          /* answers shorter than the server's block */
} net_t;

/* zigbee_ota.cpp's settings (BLOCK_RETRY_MS, BLOCK_RETRIES) on five networks,
 * the fifth with answers of any length up to the block. Last a stress
 * setting as on the bench: a 300 ms retry on a sleepy device, whose answers
 * often take longer, so many blocks are asked for twice or more. */
static const net_t nets[] = {
    {"rare late", 10000, 3, 30, 80, 0, 2, 0, 0},
    {"late", 10000, 3, 30, 80, 0, 20, 0, 0},
    {"lossy", 10000, 3, 30, 80, 0, 0, 20, 0},
    {"late+lossy", 10000, 3, 30, 80, 0, 20, 20, 0},
    {"short", 10000, 3, 30, 80, 0, 20, 20, 50},
    {"stress", 300, 20, 80, 250, 100, 0, 5, 0},
};

static uint64_t rng_state;

static uint32_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t) ((rng_state * 2685821657736338717ULL) >> 32);
}

static uint32_t uniform(uint32_t lo, uint32_t hi)
{
    return lo + rnd() % (hi - lo + 1);
}

static int chance(uint32_t per_mille)
{
    return rnd() % 1000 < per_mille;
}

enum { EV_REQ, EV_RSP, EV_RETRY };

typedef struct {
    uint32_t t; /* ms */
    uint32_t seq;
    int kind;
    uint32_t val;
} event_t;

#define MAX_EVENTS 65536
static event_t g_heap[MAX_EVENTS];
static int g_n;
static uint32_t g_seq;

static int before(const event_t *a, const event_t *b)
{
    return a->t != b->t ? a->t < b->t : a->seq < b->seq;
}

static void push(uint32_t t, int kind, uint32_t val)
{
    int i = g_n++;

    if (g_n > MAX_EVENTS) {
        printf("event queue full\n");
        exit(2);
    }
    g_heap[i] = (event_t) {t, g_seq++, kind, val};
    while (i > 0 && before(&g_heap[i], &g_heap[(i - 1) / 2])) {
        event_t tmp = g_heap[i];
        g_heap[i] = g_heap[(i - 1) / 2];
        g_heap[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}

static event_t pop(void)
{
    event_t top = g_heap[0];
    int i = 0;

    g_heap[0] = g_heap[--g_n];
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < g_n && before(&g_heap[l], &g_heap[m]))
            m = l;
        if (r < g_n && before(&g_heap[r], &g_heap[m]))
            m = r;
        if (m == i)
            break;
        event_t tmp = g_heap[i];
        g_heap[i] = g_heap[m];
        g_heap[m] = tmp;
        i = m;
    }
    return top;
}

typedef struct {
    int ok;
    uint32_t ms;
    uint32_t answers;
    uint32_t repeats;
    uint32_t gaps;
    const char *why;
} result_t;

/* Delivery time on a path that keeps the order of its frames. */
static uint32_t fifo(uint32_t *last, uint32_t t)
{
    *last = t > *last + 2 ? t : *last + 2;
    return *last;
}

/*
 * One download. Frames on the normal path keep their order; a late answer
 * overtakes nothing but arrives after the stack's retry. z2m: herdsman's
 * OtaSession waits for one request and answers it after a pause. Every
 * request that comes in the meantime is dropped. A late answer keeps it busy
 * for 3 to 8 s, as if the adapter kept retrying after herdsman gave up. Each
 * answer is checked to continue the data at the right byte.
 */
static result_t download(int z2m, uint32_t size, uint32_t block, uint64_t seed, const net_t *net)
{
    ota_blocks_t b;
    result_t res = {0, 0, 0, 0, 0, "no answer (stack gave up)"};
    uint32_t fo = 0, tries = 0, retry_gen = 0, now = 0;
    uint32_t up = 0, down = 0;             /* FIFO: last delivery time per direction */
    uint32_t busy_until = 0, last_rsp = 0; /* z2m */

    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1;
    g_n = 0;
    g_seq = 0;
    ota_blocks_start(&b, size, DEV_MAX_LEN);

#define SEND_REQUEST()                                                     \
    do {                                                                   \
        push(now + net->retry_ms, EV_RETRY, ++retry_gen);                  \
        if (!chance(net->lost_pm))                                         \
            push(fifo(&up, now + uniform(10, 30)), EV_REQ, fo);            \
    } while (0)

    SEND_REQUEST();
    while (g_n > 0) {
        event_t ev = pop();
        now = ev.t;
        if (now > 6 * 3600 * 1000) {
            res.why = "longer than 6 h";
            break;
        }
        if (ev.kind == EV_RETRY) {
            if (ev.val != retry_gen)
                continue;
            if (++tries > net->retries)
                break;
            SEND_REQUEST();
        } else if (ev.kind == EV_REQ) {
            uint32_t start = now;
            if (z2m) {
                if (now < busy_until)
                    continue; /* nobody waits for it */
                start = last_rsp + Z2M_PAUSE_MS > now ? last_rsp + Z2M_PAUSE_MS : now;
                last_rsp = start;
            }
            if (chance(net->late_pm)) {
                if (z2m)
                    busy_until = start + uniform(3000, 8000);
                push(start + uniform(10500, 14000), EV_RSP, ev.val);
            } else {
                uint32_t t = start + uniform(net->lo_ms, net->hi_ms);
                if (chance(net->slow_pm))
                    t = start + uniform(300, 900);
                if (z2m)
                    busy_until = start + uniform(20, 60);
                push(fifo(&down, t), EV_RSP, ev.val);
            }
        } else {
            uint32_t off = ev.val;
            uint32_t len = off < size ? (size - off < block ? size - off : block) : 0;
            uint32_t written = b.written;
            ota_block_step_t s;

            if (len > 1 && chance(net->short_pm))
                len = uniform(1, len - 1);

            res.answers++;
            tries = 0;
            retry_gen++; /* the answer stops the retry timer */
            fo += len;
            if (fo < size)
                SEND_REQUEST();
            s = ota_blocks_answer(&b, off, len, fo);
            if (s.take > 0 && (off + s.skip != written || s.skip + s.take != len)) {
                res.why = "bytes taken from the wrong place";
                break;
            }
            if (s.take == 0)
                (s.gap ? res.gaps++ : res.repeats++);
            if (s.give_up) {
                res.why = "gave up: too many answers out of place";
                break;
            }
            if (s.set_offset)
                fo = s.offset;
            if (fo >= size) {
                res.ok = b.written == size;
                res.why = res.ok ? "" : "ended before the data was complete";
                break;
            }
        }
    }
#undef SEND_REQUEST
    res.ms = now;
    return res;
}

static void test_downloads(void)
{
    static const uint32_t sizes[] = {9521, 9500, 35188, 356592};
    char what[200];

    printf("[3] whole downloads, 50 byte blocks\n");
    for (size_t ni = 0; ni < sizeof(nets) / sizeof(nets[0]); ni++) {
        for (int z2m = 0; z2m <= 1; z2m++) {
            uint32_t repeats = 0, gaps = 0, answers = 0, bytes = 0;
            int runs = 0, ok = 0;

            for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
                int seeds = sizes[si] > 100000 ? 20 : 100;
                for (int seed = 0; seed < seeds; seed++) {
                    result_t r = download(z2m, sizes[si], 50, (uint64_t) seed, &nets[ni]);
                    runs++;
                    ok += r.ok;
                    repeats += r.repeats;
                    gaps += r.gaps;
                    answers += r.answers;
                    bytes += sizes[si];
                    if (!r.ok || g_verbose)
                        printf("  %s %s size %u seed %d: %s %u answers, %u repeats, %u gaps, %u s %s\n",
                               nets[ni].name, z2m ? "z2m" : "zha", (unsigned) sizes[si], seed,
                               r.ok ? "ok" : "FAILED", (unsigned) r.answers, (unsigned) r.repeats,
                               (unsigned) r.gaps, (unsigned) (r.ms / 1000), r.why);
                }
            }
            snprintf(what, sizeof(what),
                     "%-10s %s: %d/%d downloads complete, %u repeats + %u gaps skipped, %.2f answers per block",
                     nets[ni].name, z2m ? "z2m" : "zha", ok, runs, (unsigned) repeats, (unsigned) gaps,
                     (double) answers / ((double) bytes / 50));
            printf("  %s\n", what);
            check(ok == runs, what);
            if (nets[ni].late_pm > 2 || nets[ni].retry_ms < 1000)
                check(repeats > 0, "the model produces repeats");
            if (z2m && nets[ni].late_pm > 2)
                check(gaps > 0, "the model produces gaps on z2m");
        }
    }
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

    printf("=== ota_blocks ===\n");
    test_single();
    test_end();
    test_recorded();
    test_in_flight();
    test_downloads();

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("ota_blocks: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
