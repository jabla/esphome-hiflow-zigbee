/*
 * ota_blocks.c — see ota_blocks.h.
 *
 * The stack's behaviour comes from a disassembly of ota_upgrade_cli.c.obj
 * (esp-zigbee-lib 2.0.4): the block answer handler stops the retry timer and
 * checks manufacturer, image type and version, not the offset. It adds the
 * answer's length to FileOffset, then sends the next request or, once
 * FileOffset >= the file size, ends the download. All of that happens before
 * the RECEIVING callback. The end of the download relies on that order and
 * on ">=": both hold on 2.0.5 hardware (a download through ZHA and one through
 * Zigbee2MQTT end on the answer after FileOffset was set to size - 1). Check
 * them again after a library update.
 */
#include "ota_blocks.h"

#include <string.h>

#define NO_REQUEST UINT32_MAX

void ota_blocks_start(ota_blocks_t *b, uint32_t file_size, uint32_t max_len)
{
    memset(b, 0, sizeof(*b));
    memset(b->sent, 0xFF, sizeof(b->sent)); /* OTA_BLOCKS_ANSWERED */
    b->file_size = file_size;
    b->max_len = max_len;
}

/* The oldest (or newest) open request for `offset`: its number, or NO_REQUEST. */
static uint32_t find_open(const ota_blocks_t *b, uint32_t offset, bool newest)
{
    uint32_t first = b->sent_count > OTA_BLOCKS_TRACKED ? b->sent_count - OTA_BLOCKS_TRACKED : 0;
    uint32_t found = NO_REQUEST;

    for (uint32_t n = first; n < b->sent_count; n++) {
        if (b->sent[n % OTA_BLOCKS_TRACKED] == offset) {
            found = n;
            if (!newest)
                break;
        }
    }
    return found;
}

ota_block_step_t ota_blocks_answer(ota_blocks_t *b, uint32_t offset, uint32_t len, uint32_t file_offset)
{
    ota_block_step_t step = {0};
    const bool running = b->file_size > 0 && file_offset < b->file_size;
    uint32_t request, pending, longest, target;

    /* Which request this answers (the oldest open one for its offset). */
    request = find_open(b, offset, false);
    if (request != NO_REQUEST)
        b->sent[request % OTA_BLOCKS_TRACKED] = OTA_BLOCKS_ANSWERED;
    /* The request the stack sent before this callback, unless it ended the
     * download instead. */
    if (running)
        b->sent[b->sent_count++ % OTA_BLOCKS_TRACKED] = file_offset;

    if (len > b->block_len)
        b->block_len = len;

    if (offset <= b->written && b->written - offset < len) {
        step.skip = b->written - offset;
        step.take = len - step.skip;
        b->written += step.take;
        b->misses = 0;
        pending = NO_REQUEST;
    } else {
        step.gap = offset > b->written;
        /* An answer to a request older than the newest open one for the
         * missing block: that block's answer is still to come. */
        pending = find_open(b, b->written, true);
        step.explained = b->written >= b->file_size ||
                         (pending != NO_REQUEST && request != NO_REQUEST && pending > request);
        if (!step.explained && ++b->misses > OTA_BLOCKS_MAX_MISSES) {
            step.give_up = true;
            return step;
        }
    }

    if (!running)
        return step;
    /* No answer is longer than this, the server's own limit aside. */
    longest = b->max_len > b->block_len ? b->max_len : b->block_len;
    if (b->written >= b->file_size) {
        /* Complete: the answer to the request just sent ends the download,
         * whatever it holds. */
        target = b->file_size - 1;
    } else if (file_offset + longest >= b->file_size) {
        /* One block behind: the next full answer, a repeat included, makes
         * the stack ask for the data's end. None can end the download. */
        target = b->written > b->block_len ? b->written - b->block_len : 0;
    } else if (step.gap && (pending == NO_REQUEST || (request != NO_REQUEST && pending < request))) {
        /* No request for the missing block, or one that was overtaken and so
         * got lost: ask again. */
        target = b->written > b->block_len ? b->written - b->block_len : 0;
    } else {
        return step;
    }
    if (target != file_offset) {
        step.set_offset = true;
        step.offset = target;
    }
    return step;
}
