/*
 * ota_blocks.h — places the block answers of the Zigbee OTA download and
 * keeps the OTA client's FileOffset where it has to point.
 *
 * esp-zigbee-lib's client does not look at the offset of a block answer. It
 * adds every answer's length to FileOffset (attribute 0x0001), then asks for
 * the next block from there or, once FileOffset reaches the file size, ends
 * the download. Only after that does the application see the answer. Two
 * things break that:
 *
 *   - A repeat: the answer to a request came after the stack's retry, so both
 *     answers arrive. FileOffset runs a block ahead with two requests in
 *     flight. ZHA answers both, so the data still comes in order.
 *   - A gap: Zigbee2MQTT takes one request at a time and drops a request that
 *     comes while it answers another. After a repeat one of the two requests
 *     goes unanswered, so a block from beyond the data arrives.
 *
 * An answer that does not continue the data is skipped. The requests the
 * stack sends after an answer are recorded (FileOffset at the callback is the
 * offset just asked for). Answers come in the order of their requests, so a
 * gap whose request went out after the last open request for the missing
 * block means that request got lost. FileOffset then goes one block behind
 * the data: the next answer makes the stack ask for the missing block again.
 * Answers to older requests (with several requests in flight) leave it alone.
 * A repeat leaves FileOffset ahead: ZHA answers the request already sent for
 * the next block; with Zigbee2MQTT the dropped one shows up as a gap. Near
 * the end FileOffset stays a block behind the data as long as data is
 * missing, so no answer can end the download early; once the data is
 * complete, any answer still on its way ends it.
 */
#ifndef ZIGBEE_OTA_BLOCKS_H
#define ZIGBEE_OTA_BLOCKS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Answers in a row that do not continue the data before the download gives
 * up. An answer to a request older than the newest open request for the
 * missing block does not count: its turn simply came first. */
#define OTA_BLOCKS_MAX_MISSES 32
/* Requests remembered, the ones in flight among them. */
#define OTA_BLOCKS_TRACKED 128
/* A remembered request whose answer came. */
#define OTA_BLOCKS_ANSWERED UINT32_MAX

typedef struct {
    uint32_t file_size;
    uint32_t max_len;   /* no answer is longer: the MaximumDataSize asked for */
    uint32_t written;   /* bytes of the file taken, in order */
    uint32_t block_len; /* longest answer so far: the server's block size */
    uint32_t misses;    /* answers in a row that did not continue the data */
    /* Requests sent after an answer: request number n in sent[n % TRACKED],
     * OTA_BLOCKS_ANSWERED once its answer came. */
    uint32_t sent[OTA_BLOCKS_TRACKED];
    uint32_t sent_count;
} ota_blocks_t;

typedef struct {
    uint32_t skip;    /* leading bytes of the answer taken before */
    uint32_t take;    /* bytes that continue the data; 0: answer skipped */
    bool gap;         /* the answer lies beyond the data */
    bool explained;   /* skipped as expected: the answer to an older request,
                         or one after the data is complete */
    bool set_offset;  /* FileOffset has to become `offset` */
    uint32_t offset;
    bool give_up;     /* OTA_BLOCKS_MAX_MISSES exceeded */
} ota_block_step_t;

/* `max_len`: the MaximumDataSize of the requests, 0 if unknown. */
void ota_blocks_start(ota_blocks_t *b, uint32_t file_size, uint32_t max_len);

/*
 * One block answer: `len` bytes from `offset` of the file. `file_offset` is
 * FileOffset as the stack left it, after counting this answer. Counts the
 * `take` bytes as written; the caller writes them (answer bytes `skip` to
 * `skip + take`).
 */
ota_block_step_t ota_blocks_answer(ota_blocks_t *b, uint32_t offset, uint32_t len, uint32_t file_offset);

#ifdef __cplusplus
}
#endif

#endif /* ZIGBEE_OTA_BLOCKS_H */
