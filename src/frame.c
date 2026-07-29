/* Message framing.
 *
 * A length prefix from a peer is an instruction to allocate, so it is bounded
 * before it is believed.  The reader is incremental because a socket delivers
 * whatever it feels like: a message may arrive in one read, in fragments, or
 * with three others behind it. */
#include "kilix_mux.h"

#include <string.h>

static kmx_result
put_varint(kmx_buffer *out, uint64_t value) {
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

/* Returns KMX_ERR_TRUNCATED when the buffer does not yet hold a whole varint,
 * which for an incremental reader means "wait", not "fail". */
static kmx_result
peek_varint(
    const unsigned char *data,
    size_t size,
    size_t *offset,
    uint64_t *value
) {
    uint64_t result = 0;
    unsigned shift = 0;
    size_t cursor = *offset;
    while (true) {
        unsigned char byte;
        if (cursor >= size) return KMX_ERR_TRUNCATED;
        byte = data[cursor++];
        if (shift > 63) return KMX_ERR_PROTOCOL;
        result |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) break;
        shift += 7;
    }
    *offset = cursor;
    *value = result;
    return KMX_OK;
}

kmx_result
kmx_frame_encode(
    kmx_message_type type,
    const void *payload,
    size_t size,
    kmx_buffer *out
) {
    unsigned char kind = (unsigned char)type;
    kmx_result result;
    if (!out || (!payload && size)) return KMX_ERR_INVALID;
    if (size > KMX_MESSAGE_MAX) return KMX_ERR_LIMIT;
    result = put_varint(out, size + 1);
    if (result == KMX_OK) result = kmx_buffer_append(out, &kind, 1);
    if (result == KMX_OK) result = kmx_buffer_append(out, payload, size);
    return result;
}

void
kmx_framer_init(kmx_framer *framer) {
    if (framer) kmx_buffer_init(&framer->pending);
}

void
kmx_framer_free(kmx_framer *framer) {
    if (framer) kmx_buffer_free(&framer->pending);
}

kmx_result
kmx_framer_push(kmx_framer *framer, const void *data, size_t size) {
    if (!framer) return KMX_ERR_INVALID;
    if (framer->pending.size + size > KMX_MESSAGE_MAX * 2u) return KMX_ERR_LIMIT;
    return kmx_buffer_append(&framer->pending, data, size);
}

kmx_result
kmx_framer_next(
    kmx_framer *framer,
    bool *ready,
    kmx_message_type *type,
    const unsigned char **payload,
    size_t *size
) {
    size_t offset = 0;
    uint64_t length;
    kmx_result result;

    if (!framer || !ready || !type || !payload || !size) return KMX_ERR_INVALID;
    *ready = false;
    result = peek_varint(framer->pending.data, framer->pending.size, &offset, &length);
    if (result == KMX_ERR_TRUNCATED) return KMX_OK;
    if (result != KMX_OK) return result;
    if (!length || length > KMX_MESSAGE_MAX + 1u) return KMX_ERR_LIMIT;
    if (framer->pending.size - offset < length) return KMX_OK;

    *type = (kmx_message_type)framer->pending.data[offset];
    *payload = framer->pending.data + offset + 1;
    *size = (size_t)length - 1;
    *ready = true;
    return KMX_OK;
}

void
kmx_framer_consume(kmx_framer *framer) {
    size_t offset = 0;
    uint64_t length;
    if (!framer) return;
    if (peek_varint(
            framer->pending.data, framer->pending.size, &offset, &length) != KMX_OK) {
        return;
    }
    if (framer->pending.size - offset < length) return;
    memmove(
        framer->pending.data,
        framer->pending.data + offset + length,
        framer->pending.size - offset - (size_t)length);
    framer->pending.size -= offset + (size_t)length;
}
