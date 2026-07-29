/* State synchronisation and wire framing.
 *
 * The sender holds two things: the state the receiver has acknowledged, and
 * the state the pane is in now.  Every message is the difference between them.
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

/* How many sent-but-unacknowledged states to remember.  Bounded because an
 * unbounded history is the queue this design exists to avoid. */
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
kmx_decompress(const void *data, size_t size, kmx_buffer *out) {
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
     * cell codec could ever legitimately produce. */
    if (raw_size > (uint64_t)KMX_MAX_CELLS * 16u) return KMX_ERR_LIMIT;

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
    bool used;
} sent_state;

struct kmx_sync {
    kmx_term *term;
    bool owns_term;
    kmx_grid current;
    kmx_grid acked;
    bool acked_valid;
    sent_state history[KMX_SENT_HISTORY];
    uint64_t next_sequence;
    uint64_t last_send_millis;
    unsigned interval_millis;
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
}

void
kmx_sync_reset_baseline(kmx_sync *sync) {
    size_t index;
    if (!sync) return;
    sync->acked_valid = false;
    sync->last_send_millis = 0;
    for (index = 0; index < KMX_SENT_HISTORY; index++) {
        sync->history[index].used = false;
    }
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
history_slot(kmx_sync *sync, uint64_t sequence) {
    return &sync->history[sequence % KMX_SENT_HISTORY];
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

    if (!sync || !out || !produced) return KMX_ERR_INVALID;
    *produced = false;

    result = kmx_term_snapshot(sync->term, &sync->current);
    if (result != KMX_OK) return result;

    /* Nothing to say costs nothing.  This is what an idle session is. */
    if (sync->acked_valid && kmx_grid_equal(&sync->acked, &sync->current)) {
        return KMX_OK;
    }
    if (sync->last_send_millis &&
        now_millis - sync->last_send_millis < sync->interval_millis) {
        return KMX_OK;
    }

    from_scratch = !sync->acked_valid ||
        sync->acked.rows != sync->current.rows ||
        sync->acked.cols != sync->current.cols;

    kmx_buffer_init(&encoded);
    result = kmx_cells_encode(
        from_scratch ? NULL : &sync->acked, &sync->current, &encoded);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }

    sequence = sync->next_sequence++;
    result = put_varint_buffer(out, sequence);
    if (result == KMX_OK) result = kmx_compress(encoded.data, encoded.size, out);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }

    /* Remember what this sequence claimed, so its acknowledgement can move the
     * baseline forward without a round trip's worth of guessing. */
    slot = history_slot(sync, sequence);
    result = kmx_grid_copy(&slot->state, &sync->current);
    if (result != KMX_OK) {
        kmx_buffer_free(&encoded);
        return result;
    }
    slot->sequence = sequence;
    slot->used = true;

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

kmx_result
kmx_sync_ack(kmx_sync *sync, uint64_t sequence) {
    sent_state *slot;
    if (!sync) return KMX_ERR_INVALID;
    if (sequence == 0 || sequence >= sync->next_sequence) return KMX_ERR_INVALID;
    slot = history_slot(sync, sequence);
    /* An acknowledgement for a state that has aged out of the ring is not an
     * error; the baseline simply stays where it is and the next message is a
     * larger diff. */
    if (!slot->used || slot->sequence != sequence) return KMX_OK;
    if (kmx_grid_copy(&sync->acked, &slot->state) != KMX_OK) return KMX_ERR_MEMORY;
    sync->acked_valid = true;
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
    result = kmx_decompress(bytes + offset, size - offset, &plain);
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
