/* State synchronisation and wire framing.
 *
 * The sender holds a retained baseline covered by the receiver's cumulative
 * acknowledgement, and the state the pane is in now. Every message is the
 * difference between them, plus repairs for possible intermediate screens.
 * Rows changed in an unacknowledged state are included even if they have since
 * reverted to the baseline. A receiver on the ordered stream may already hold
 * an intermediate screen, rather than the acknowledged one.
 * Nothing is queued, so a link that was down for a minute costs one screen
 * when it returns rather than a minute of scrollback, and a message that was
 * lost is superseded by the next one rather than retransmitted.
 *
 * The design is mosh's; the code is not.  mosh is GPL-3 and this is MIT, so it
 * was read for its ideas and reimplemented. */
#include "kilix_mux.h"

#include <zstd.h>

#include <stdlib.h>
#include <string.h>

/* Wire framing:  [seq varint][codec byte][raw size varint][payload] */
#define KMX_CODEC_RAW 0u
#define KMX_CODEC_ZSTD 1u

/* How many sent-but-unacknowledged states to remember. Preserve the oldest
 * unacknowledged state as an anchor; rotating every slot would evict all ACK
 * evidence on a link whose RTT exceeds eight send intervals. The other slots
 * retain recent screens, so skipped messages never stop fresh updates. */
#define KMX_SENT_HISTORY 8

static kmx_result
put_varint_buffer(kmx_buffer *out, uint64_t value) {
    unsigned char scratch[10];
    size_t used = 0;
    do {
        unsigned char byte = (unsigned char)(value & 0x7fu);
        value >>= 7;
        if (value) byte |= 0x80u;
        scratch[used++] = byte;
    } while (value);
    return kmx_buffer_append(out, scratch, used);
}

static kmx_result
get_varint_buffer(
    const unsigned char *data,
    size_t size,
    size_t *offset,
    uint64_t *value
) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        unsigned char byte;
        if (*offset >= size) return KMX_ERR_TRUNCATED;
        byte = data[(*offset)++];
        if (shift > 63) return KMX_ERR_PROTOCOL;
        result |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) break;
        shift += 7;
    }
    *value = result;
    return KMX_OK;
}

kmx_result
kmx_compress(const void *data, size_t size, kmx_buffer *out) {
    size_t bound;
    size_t produced;
    unsigned char codec;
    kmx_result result;
    if ((!data && size) || !out) return KMX_ERR_INVALID;

    bound = ZSTD_compressBound(size);
    if (ZSTD_isError(bound)) return KMX_ERR_LIMIT;
    {
        unsigned char *scratch = malloc(bound ? bound : 1);
        if (!scratch) return KMX_ERR_MEMORY;
        produced = ZSTD_compress(scratch, bound, data, size, 3);
        if (ZSTD_isError(produced)) {
            free(scratch);
            return KMX_ERR_LIMIT;
        }
        /* Compression that does not pay for itself is not used.  Framing
         * records the choice, so an incompressible message costs one byte
         * instead of growing. */
        if (produced >= size) {
            free(scratch);
            codec = KMX_CODEC_RAW;
            result = kmx_buffer_append(out, &codec, 1);
            if (result == KMX_OK) result = put_varint_buffer(out, size);
            if (result == KMX_OK) result = kmx_buffer_append(out, data, size);
            return result;
        }
        codec = KMX_CODEC_ZSTD;
        result = kmx_buffer_append(out, &codec, 1);
        if (result == KMX_OK) result = put_varint_buffer(out, size);
        if (result == KMX_OK) result = kmx_buffer_append(out, scratch, produced);
        free(scratch);
    }
    return result;
}

kmx_result
kmx_decompress(const void *data, size_t size, kmx_buffer *out, size_t limit) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    uint64_t raw_size;
    unsigned char codec;
    kmx_result result;

    if ((!data && size) || !out) return KMX_ERR_INVALID;
    if (size < 1) return KMX_ERR_TRUNCATED;
    codec = bytes[offset++];
    result = get_varint_buffer(bytes, size, &offset, &raw_size);
    if (result != KMX_OK) return result;
    /* The declared size drives an allocation, so it is bounded by what the
     * caller says its plane can legitimately produce. */
    if (!limit || raw_size > (uint64_t)limit) return KMX_ERR_LIMIT;

    if (codec == KMX_CODEC_RAW) {
        if (size - offset != raw_size) return KMX_ERR_PROTOCOL;
        return kmx_buffer_append(out, bytes + offset, (size_t)raw_size);
    }
    if (codec != KMX_CODEC_ZSTD) return KMX_ERR_PROTOCOL;
    {
        unsigned char *scratch = malloc((size_t)raw_size ? (size_t)raw_size : 1);
        size_t produced;
        if (!scratch) return KMX_ERR_MEMORY;
        produced = ZSTD_decompress(
            scratch, (size_t)raw_size, bytes + offset, size - offset);
        if (ZSTD_isError(produced) || produced != raw_size) {
            free(scratch);
            return KMX_ERR_PROTOCOL;
        }
        result = kmx_buffer_append(out, scratch, produced);
        free(scratch);
    }
    return result;
}

/* ---- sender ----------------------------------------------------------- */

typedef struct {
    uint64_t sequence;
    kmx_grid state;
    uint64_t sent_at;
    size_t wire_bytes;
    bool used;
} sent_state;

struct kmx_sync {
    kmx_term *term;
    bool owns_term;
    kmx_grid current;
    kmx_grid acked;
    bool acked_valid;
    /* The highest sequence the baseline has been moved to.  Acknowledgements
     * are cumulative, so this only ever moves forward; see kmx_sync_ack_at. */
    uint64_t acked_sequence;
    /* Most recent emitted change to each row, relative to the preceding sent
     * screen. These obligations survive history eviction. Repeating the same
     * row does not advance its change sequence, so an ACK of an identical
     * screen can still make the sender idle with newer repeats in flight. */
    uint64_t row_changed_at[KMX_MAX_DIMENSION];
    uint64_t cursor_changed_at;
    sent_state history[KMX_SENT_HISTORY];
    uint64_t next_sequence;
    uint64_t last_send_millis;
    unsigned interval_millis;
    bool interval_pinned;
    unsigned smoothed_rtt;
    uint64_t acked_bytes;
    uint64_t acked_span;
};

static kmx_result
sync_init(kmx_sync **out, kmx_term *term, bool owns, int rows, int cols) {
    kmx_sync *sync;
    kmx_result result;
    size_t index;
    if (!out) return KMX_ERR_INVALID;
    sync = calloc(1, sizeof *sync);
    if (!sync) return KMX_ERR_MEMORY;
    sync->owns_term = owns;
    sync->term = term;
    result = term ? KMX_OK : kmx_term_create(&sync->term, rows, cols);
    if (result == KMX_OK) result = kmx_grid_init(&sync->current, rows, cols);
    if (result == KMX_OK) result = kmx_grid_init(&sync->acked, rows, cols);
    for (index = 0; index < KMX_SENT_HISTORY && result == KMX_OK; index++) {
        result = kmx_grid_init(&sync->history[index].state, rows, cols);
    }
    if (result != KMX_OK) {
        kmx_sync_free(sync);
        return result;
    }
    sync->next_sequence = 1;
    sync->interval_millis = KMX_SEND_INTERVAL_MIN_MS;
    *out = sync;
    return KMX_OK;
}

kmx_result
kmx_sync_create(kmx_sync **out, int rows, int cols) {
    return sync_init(out, NULL, true, rows, cols);
}

kmx_result
kmx_sync_create_over(kmx_sync **out, kmx_term *term) {
    kmx_grid probe;
    kmx_result result;
    if (!out || !term) return KMX_ERR_INVALID;
    /* Take the dimensions from the terminal itself rather than asking the
     * caller to repeat them, so the two cannot disagree. */
    memset(&probe, 0, sizeof probe);
    result = kmx_term_snapshot(term, &probe);
    if (result != KMX_OK) return result;
    result = sync_init(out, term, false, probe.rows, probe.cols);
    kmx_grid_free(&probe);
    return result;
}

void
kmx_sync_free(kmx_sync *sync) {
    size_t index;
    if (!sync) return;
    if (sync->owns_term) kmx_term_free(sync->term);
    kmx_grid_free(&sync->current);
    kmx_grid_free(&sync->acked);
    for (index = 0; index < KMX_SENT_HISTORY; index++) {
        kmx_grid_free(&sync->history[index].state);
    }
    free(sync);
}

kmx_result
kmx_sync_feed(kmx_sync *sync, const void *data, size_t size) {
    if (!sync) return KMX_ERR_INVALID;
    return kmx_term_feed(sync->term, data, size);
}

kmx_result
kmx_sync_resize(kmx_sync *sync, int rows, int cols) {
    if (!sync) return KMX_ERR_INVALID;
    return kmx_term_resize(sync->term, rows, cols);
}

void
kmx_sync_set_interval(kmx_sync *sync, unsigned millis) {
    if (!sync) return;
    if (millis < KMX_SEND_INTERVAL_MIN_MS) millis = KMX_SEND_INTERVAL_MIN_MS;
    if (millis > KMX_SEND_INTERVAL_MAX_MS) millis = KMX_SEND_INTERVAL_MAX_MS;
    sync->interval_millis = millis;
    sync->interval_pinned = true;
}

unsigned
kmx_sync_rtt_millis(const kmx_sync *sync) {
    return sync ? sync->smoothed_rtt : 0;
}

unsigned
kmx_sync_interval_millis(const kmx_sync *sync) {
    return sync ? sync->interval_millis : 0;
}

uint32_t
kmx_sync_throughput(const kmx_sync *sync) {
    if (!sync || !sync->acked_span || !sync->acked_bytes) return 0;
    return (uint32_t)((sync->acked_bytes * 1000u) / sync->acked_span);
}

void
kmx_sync_reset_baseline(kmx_sync *sync) {
    size_t index;
    if (!sync) return;
    sync->acked_valid = false;
    sync->last_send_millis = 0;
    /* Everything sent so far belongs to whoever was attached before.  Moving
     * the floor up to the last issued sequence means a late acknowledgement
     * from that session cannot install a baseline the newly attached client
     * has never held, while every sequence issued from here on still
     * advances normally. */
    sync->acked_sequence =
        sync->next_sequence ? sync->next_sequence - 1 : 0;
    for (index = 0; index < KMX_SENT_HISTORY; index++) {
        sync->history[index].used = false;
    }
    memset(sync->row_changed_at, 0, sizeof sync->row_changed_at);
    sync->cursor_changed_at = 0;
}

const kmx_grid *
kmx_sync_current(const kmx_sync *sync) {
    return sync ? &sync->current : NULL;
}

kmx_term *
kmx_sync_term(kmx_sync *sync) {
    return sync ? sync->term : NULL;
}

static sent_state *
history_at_or_before(kmx_sync *sync, uint64_t sequence) {
    sent_state *best = NULL;
    for (size_t index = 0; index < KMX_SENT_HISTORY; index++) {
        sent_state *slot = &sync->history[index];
        if (slot->used && slot->sequence <= sequence &&
            (!best || slot->sequence > best->sequence)) best = slot;
    }
    return best;
}

static sent_state *
history_next_slot(kmx_sync *sync) {
    sent_state *oldest = NULL;
    sent_state *second = NULL;
    for (size_t index = 0; index < KMX_SENT_HISTORY; index++) {
        sent_state *slot = &sync->history[index];
        if (!slot->used || slot->sequence <= sync->acked_sequence) return slot;
        if (!oldest || slot->sequence < oldest->sequence) {
            second = oldest;
            oldest = slot;
        } else if (!second || slot->sequence < second->sequence) {
            second = slot;
        }
    }
    /* A full history has eight unacknowledged states. Keep its oldest anchor
     * until a cumulative ACK covers it and replace the next oldest instead. */
    return second;
}

static bool
same_row(const kmx_grid *a, const kmx_grid *b, int row) {
    if (!a || a->rows != b->rows || a->cols != b->cols) return false;
    for (int col = 0; col < b->cols; col++) {
        if (!kmx_cell_equal(kmx_grid_cell_const(a, row, col),
                            kmx_grid_cell_const(b, row, col))) return false;
    }
    return true;
}

kmx_result
kmx_sync_poll(
    kmx_sync *sync,
    uint64_t now_millis,
    kmx_buffer *out,
    bool *produced,
    kmx_sync_info *info
) {
    kmx_buffer encoded;
    kmx_result result;
    uint64_t sequence;
    sent_state *slot;
    bool from_scratch;
    bool changed_rows[KMX_MAX_DIMENSION];
    bool repair_rows[KMX_MAX_DIMENSION];
    bool repair_pending = false;
    bool changed_since_send = false;
    bool cursor_changed;
    const kmx_grid *last_sent = NULL;

    if (!sync || !out || !produced) return KMX_ERR_INVALID;
    *produced = false;

    result = kmx_term_snapshot(sync->term, &sync->current);
    if (result != KMX_OK) return result;

    if (sync->next_sequence > 1) {
        sent_state *last = history_at_or_before(sync, sync->next_sequence - 1);
        if (last && last->sequence == sync->next_sequence - 1) {
            last_sent = &last->state;
        }
    }
    for (int row = 0; row < sync->current.rows; row++) {
        changed_rows[row] = !same_row(last_sent, &sync->current, row);
        changed_since_send |= changed_rows[row];
        repair_rows[row] = changed_rows[row] ||
            sync->row_changed_at[row] > sync->acked_sequence;
        repair_pending |= repair_rows[row];
    }
    cursor_changed = !last_sent ||
        last_sent->cursor_row != sync->current.cursor_row ||
        last_sent->cursor_col != sync->current.cursor_col ||
        last_sent->cursor_visible != sync->current.cursor_visible;
    /* Cursor fields are always sent, but a cursor-only reversion must also
     * defeat idle suppression while the intermediate cursor is unacked. */
    repair_pending |= cursor_changed ||
        sync->cursor_changed_at > sync->acked_sequence;
    changed_since_send |= cursor_changed;

    /* Nothing to say costs nothing.  This is what an idle session is. */
    if (sync->acked_valid && !repair_pending &&
        kmx_grid_equal(&sync->acked, &sync->current)) {
        return KMX_OK;
    }
    {
        unsigned interval = sync->interval_millis;
        /* An unchanged retry should allow its ACK to arrive. Repeating it at
         * the repaint cadence wastes bandwidth and pushes the next keystroke's
         * changed screen behind another interval. Keep retries for skipped
         * messages, but wait two measured round trips for an identical screen.
         * Before the first sample, a one-second retry avoids filling the
         * stream with duplicate initial screens. New content still uses the
         * normal cadence; explicit pinning wins. */
        if (!sync->interval_pinned && !changed_since_send) {
            unsigned retry = sync->smoothed_rtt ? sync->smoothed_rtt * 2u : 1000u;
            if (retry > interval) interval = retry;
        }
        if (sync->last_send_millis &&
            now_millis - sync->last_send_millis < interval) return KMX_OK;
    }

    from_scratch = !sync->acked_valid ||
        sync->acked.rows != sync->current.rows ||
        sync->acked.cols != sync->current.cols;

    kmx_buffer_init(&encoded);
    result = kmx_cells_encode_rows(
        from_scratch ? NULL : &sync->acked, &sync->current, repair_rows, &encoded);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }

    sequence = sync->next_sequence;
    result = put_varint_buffer(out, sequence);
    if (result == KMX_OK) result = kmx_compress(encoded.data, encoded.size, out);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }

    /* Remember what this sequence claimed, so its acknowledgement can move the
     * baseline forward without a round trip's worth of guessing. */
    slot = history_next_slot(sync);
    result = kmx_grid_copy(&slot->state, &sync->current);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }
    slot->sequence = sequence;
    slot->sent_at = now_millis;
    slot->wire_bytes = out->size;
    slot->used = true;
    for (int row = 0; row < sync->current.rows; row++) {
        if (changed_rows[row]) sync->row_changed_at[row] = sequence;
    }
    if (cursor_changed) sync->cursor_changed_at = sequence;
    sync->next_sequence++;

    sync->last_send_millis = now_millis ? now_millis : 1;
    *produced = true;
    if (info) {
        info->sequence = sequence;
        info->raw_bytes = encoded.size;
        info->wire_bytes = out->size;
        info->from_scratch = from_scratch;
    }
    kmx_buffer_free(&encoded);
    return KMX_OK;
}

/* Fold a round-trip sample into the smoothed estimate and, unless the caller
 * pinned it, into the send interval.
 *
 * There is no point producing messages faster than the far end can
 * acknowledge them: on a slow link that only builds a backlog of screens that
 * are already stale by the time they arrive.  Half the round trip is the
 * useful rate - a message in flight each way - clamped to the bounds this
 * plane will accept.
 *
 * The weighting is the usual 7/8 smoothing: one slow sample should nudge the
 * estimate, not seize it. */
static void
observe_round_trip(kmx_sync *sync, uint64_t sample, size_t bytes) {
    if (sample > 60000u) return; /* a clock jump, not a round trip */
    sync->smoothed_rtt = sync->smoothed_rtt
        ? (unsigned)((sync->smoothed_rtt * 7u + sample) / 8u)
        : (unsigned)sample;
    sync->acked_bytes += bytes;
    sync->acked_span += sample ? sample : 1u;
    if (sync->interval_pinned) return;
    {
        unsigned wanted = sync->smoothed_rtt / 2u;
        if (wanted < KMX_SEND_INTERVAL_MIN_MS) wanted = KMX_SEND_INTERVAL_MIN_MS;
        if (wanted > KMX_SEND_INTERVAL_MAX_MS) wanted = KMX_SEND_INTERVAL_MAX_MS;
        sync->interval_millis = wanted;
    }
}

kmx_result
kmx_sync_ack(kmx_sync *sync, uint64_t sequence) {
    return kmx_sync_ack_at(sync, sequence, 0);
}

kmx_result
kmx_sync_ack_at(kmx_sync *sync, uint64_t sequence, uint64_t now_millis) {
    sent_state *slot;
    if (!sync) return KMX_ERR_INVALID;
    if (sequence == 0 || sequence >= sync->next_sequence) return KMX_ERR_INVALID;
    /* Acknowledgements are cumulative, so one that does not advance the
     * baseline has nothing to say.  Applying it anyway would move the baseline
     * BACKWARD, and that is not merely wasteful: the next diff would be
     * computed against a state the receiver has already moved past, so any
     * cell that changed and changed back between the two would match the old
     * baseline, be omitted from the diff, and leave the receiver displaying
     * the intermediate value permanently.
     *
     * A duplicate or reordered acknowledgement is ordinary traffic, not an
     * error, so this is a no-op rather than a failure.  Its round trip is not
     * folded into the estimate either; a duplicate's arrival time says nothing
     * about how long the original took. */
    if (sequence <= sync->acked_sequence) return KMX_OK;
    slot = history_at_or_before(sync, sequence);
    if (!slot || slot->sequence <= sync->acked_sequence) return KMX_OK;
    /* A cumulative ACK also covers an earlier retained screen, even when that
     * earlier message was skipped. The receiver may hold a newer screen, just
     * as it does with delayed ACKs; the row/cursor repair obligations cover
     * every change after this retained baseline. Advance only to its sequence,
     * never to the unknown evicted state, so no obligations are cleared early.
     *
     * If this is a later ACK, its age is a conservative RTT upper bound for the
     * anchor. Unlike ignoring evicted ACKs, it can bootstrap slower pacing even
     * after the anchor's own message or ACK was lost. Exact matches retain the
     * usual measured RTT sample. Duplicate/stale ACKs cannot advance again. */
    if (now_millis && now_millis >= slot->sent_at) {
        observe_round_trip(sync, now_millis - slot->sent_at, slot->wire_bytes);
    }
    if (kmx_grid_copy(&sync->acked, &slot->state) != KMX_OK) return KMX_ERR_MEMORY;
    sync->acked_valid = true;
    sync->acked_sequence = slot->sequence;
    return KMX_OK;
}

/* ---- receiver --------------------------------------------------------- */

struct kmx_receiver {
    kmx_grid grid;
};

kmx_result
kmx_receiver_create(kmx_receiver **out, int rows, int cols) {
    kmx_receiver *receiver;
    kmx_result result;
    if (!out) return KMX_ERR_INVALID;
    receiver = calloc(1, sizeof *receiver);
    if (!receiver) return KMX_ERR_MEMORY;
    result = kmx_grid_init(&receiver->grid, rows, cols);
    if (result != KMX_OK) {
        free(receiver);
        return result;
    }
    *out = receiver;
    return KMX_OK;
}

void
kmx_receiver_free(kmx_receiver *receiver) {
    if (!receiver) return;
    kmx_grid_free(&receiver->grid);
    free(receiver);
}

kmx_result
kmx_receiver_apply(
    kmx_receiver *receiver,
    const void *data,
    size_t size,
    uint64_t *sequence
) {
    const unsigned char *bytes = data;
    kmx_buffer plain;
    size_t offset = 0;
    uint64_t seq;
    kmx_result result;

    if (!receiver || (!data && size)) return KMX_ERR_INVALID;
    result = get_varint_buffer(bytes, size, &offset, &seq);
    if (result != KMX_OK) return result;
    kmx_buffer_init(&plain);
    result = kmx_decompress(
        bytes + offset, size - offset, &plain, KMX_CELLS_WIRE_MAX);
    if (result == KMX_OK) {
        result = kmx_cells_apply(&receiver->grid, plain.data, plain.size);
    }
    kmx_buffer_free(&plain);
    if (result == KMX_OK && sequence) *sequence = seq;
    return result;
}

const kmx_grid *
kmx_receiver_grid(const kmx_receiver *receiver) {
    return receiver ? &receiver->grid : NULL;
}
