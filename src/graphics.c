/* The still-graphics plane.
 *
 * An image is addressed by its content, so the second time the same picture
 * appears - a redraw, a reattach, a pane moving - it costs a sixteen-byte
 * reference instead of a transfer.  That is the whole difference between a
 * photograph in a pane costing once and costing every repaint, and it is the
 * mechanism SPICE and RDP both settled on for the same reason.
 *
 * This is the plane that a byte-forwarding multiplexer cannot have: it only
 * sees an escape sequence go past, so it must forward every byte every time. */
#include "kilix_mux.h"

#include <stdlib.h>
#include <string.h>

/* A 128-bit FNV-1a, run as two independent lanes with different offset bases
 * and the length folded in.  Non-cryptographic on purpose: this defends
 * against coincidence, not against someone choosing a collision, and both
 * ends of the connection are the same user. */
kmx_image_key
kmx_image_key_of(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint64_t low = 0xcbf29ce484222325ull;
    uint64_t high = 0x9e3779b97f4a7c15ull;
    kmx_image_key key;
    size_t index;
    for (index = 0; index < size; index++) {
        low = (low ^ bytes[index]) * 0x100000001b3ull;
        high = (high ^ (uint64_t)(bytes[index] + index)) * 0xff51afd7ed558ccdull;
        high ^= high >> 29;
    }
    low ^= (uint64_t)size * 0x9e3779b97f4a7c15ull;
    high ^= (uint64_t)size;
    for (index = 0; index < 8; index++) {
        key.bytes[index] = (unsigned char)(low >> (8 * index));
        key.bytes[index + 8] = (unsigned char)(high >> (8 * index));
    }
    return key;
}

bool
kmx_image_key_equal(const kmx_image_key *a, const kmx_image_key *b) {
    if (!a || !b) return a == b;
    return memcmp(a->bytes, b->bytes, KMX_IMAGE_KEY_BYTES) == 0;
}

/* ---- cache ------------------------------------------------------------ */

typedef struct {
    kmx_image_key key;
    unsigned char *data;
    size_t size;
    uint64_t used;   /* for eviction: higher is more recently wanted */
    bool occupied;
} entry;

struct kmx_image_cache {
    entry *entries;
    size_t capacity;
    size_t count;
    size_t bytes;
    size_t max_bytes;
    uint64_t clock;
};

kmx_result
kmx_image_cache_create(
    kmx_image_cache **out,
    size_t max_entries,
    size_t max_bytes
) {
    kmx_image_cache *cache;
    if (!out || !max_entries || !max_bytes) return KMX_ERR_INVALID;
    if (max_entries > 4096) return KMX_ERR_INVALID;
    cache = calloc(1, sizeof *cache);
    if (!cache) return KMX_ERR_MEMORY;
    cache->entries = calloc(max_entries, sizeof *cache->entries);
    if (!cache->entries) {
        free(cache);
        return KMX_ERR_MEMORY;
    }
    cache->capacity = max_entries;
    cache->max_bytes = max_bytes;
    *out = cache;
    return KMX_OK;
}

void
kmx_image_cache_free(kmx_image_cache *cache) {
    size_t index;
    if (!cache) return;
    for (index = 0; index < cache->capacity; index++) {
        free(cache->entries[index].data);
    }
    free(cache->entries);
    free(cache);
}

static entry *
find(const kmx_image_cache *cache, const kmx_image_key *key) {
    size_t index;
    for (index = 0; index < cache->capacity; index++) {
        entry *item = &cache->entries[index];
        if (item->occupied && kmx_image_key_equal(&item->key, key)) return item;
    }
    return NULL;
}

bool
kmx_image_cache_has(const kmx_image_cache *cache, const kmx_image_key *key) {
    if (!cache || !key) return false;
    return find(cache, key) != NULL;
}

const unsigned char *
kmx_image_cache_get(
    const kmx_image_cache *cache,
    const kmx_image_key *key,
    size_t *size
) {
    entry *item;
    if (!cache || !key) return NULL;
    item = find(cache, key);
    if (!item) return NULL;
    /* Touching on read is what makes the eviction order about what is still
     * being drawn rather than about what arrived first. */
    item->used = ++((kmx_image_cache *)cache)->clock;
    if (size) *size = item->size;
    return item->data;
}

static void
release(kmx_image_cache *cache, entry *item) {
    if (!item->occupied) return;
    cache->bytes -= item->size;
    cache->count--;
    free(item->data);
    item->data = NULL;
    item->size = 0;
    item->occupied = false;
}

static entry *
find_free(kmx_image_cache *cache) {
    size_t index;
    for (index = 0; index < cache->capacity; index++) {
        if (!cache->entries[index].occupied) return &cache->entries[index];
    }
    return NULL;
}

/* Deliberately separate from find_free: a loop that has to make room must be
 * sure something was actually released.  Returning a free slot without
 * evicting anything spins forever whenever slots are available but the byte
 * budget is what is short. */
static bool
evict_oldest(kmx_image_cache *cache) {
    entry *oldest = NULL;
    size_t index;
    for (index = 0; index < cache->capacity; index++) {
        entry *item = &cache->entries[index];
        if (!item->occupied) continue;
        if (!oldest || item->used < oldest->used) oldest = item;
    }
    if (!oldest) return false;
    release(cache, oldest);
    return true;
}

kmx_result
kmx_image_cache_put(
    kmx_image_cache *cache,
    const kmx_image_key *key,
    const void *data,
    size_t size
) {
    entry *slot;
    unsigned char *copy;
    if (!cache || !key || (!data && size)) return KMX_ERR_INVALID;
    if (size > cache->max_bytes) return KMX_ERR_LIMIT;
    slot = find(cache, key);
    if (slot) {
        slot->used = ++cache->clock;
        return KMX_OK;
    }
    /* Make room first, so a large image cannot briefly exceed the budget. */
    while (cache->bytes + size > cache->max_bytes) {
        if (!evict_oldest(cache)) break;
    }
    if (cache->bytes + size > cache->max_bytes) return KMX_ERR_LIMIT;
    slot = find_free(cache);
    if (!slot) {
        if (!evict_oldest(cache)) return KMX_ERR_LIMIT;
        slot = find_free(cache);
    }
    if (!slot) return KMX_ERR_LIMIT;
    copy = malloc(size ? size : 1);
    if (!copy) return KMX_ERR_MEMORY;
    memcpy(copy, data, size);
    slot->key = *key;
    slot->data = copy;
    slot->size = size;
    slot->used = ++cache->clock;
    slot->occupied = true;
    cache->count++;
    cache->bytes += size;
    return KMX_OK;
}

size_t
kmx_image_cache_count(const kmx_image_cache *cache) {
    return cache ? cache->count : 0;
}

size_t
kmx_image_cache_bytes(const kmx_image_cache *cache) {
    return cache ? cache->bytes : 0;
}

/* ---- wire form -------------------------------------------------------- */

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

static kmx_result
get_varint(
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

#define KMX_IMAGE_KIND_REF 0u
#define KMX_IMAGE_KIND_DATA 1u

kmx_result
kmx_image_encode(
    uint32_t pane,
    const kmx_image_key *key,
    const void *data,
    size_t size,
    kmx_buffer *out
) {
    unsigned char kind = data ? (unsigned char)KMX_IMAGE_KIND_DATA
                              : (unsigned char)KMX_IMAGE_KIND_REF;
    kmx_result result;
    if (!key || !out) return KMX_ERR_INVALID;
    if (data && size > KMX_MESSAGE_MAX) return KMX_ERR_LIMIT;
    result = put_varint(out, pane);
    if (result == KMX_OK) result = kmx_buffer_append(out, &kind, 1);
    if (result == KMX_OK) {
        result = kmx_buffer_append(out, key->bytes, KMX_IMAGE_KEY_BYTES);
    }
    if (result == KMX_OK && data) {
        result = put_varint(out, size);
        if (result == KMX_OK) result = kmx_buffer_append(out, data, size);
    }
    return result;
}

kmx_result
kmx_image_decode(
    const void *wire,
    size_t wire_size,
    kmx_image_message *message
) {
    const unsigned char *bytes = wire;
    size_t offset = 0;
    uint64_t pane;
    unsigned char kind;
    kmx_result result;

    if ((!wire && wire_size) || !message) return KMX_ERR_INVALID;
    memset(message, 0, sizeof *message);
    result = get_varint(bytes, wire_size, &offset, &pane);
    if (result != KMX_OK) return result;
    if (pane >= KMX_MAX_PANES) return KMX_ERR_PROTOCOL;
    if (offset >= wire_size) return KMX_ERR_TRUNCATED;
    kind = bytes[offset++];
    if (wire_size - offset < KMX_IMAGE_KEY_BYTES) return KMX_ERR_TRUNCATED;
    memcpy(message->key.bytes, bytes + offset, KMX_IMAGE_KEY_BYTES);
    offset += KMX_IMAGE_KEY_BYTES;
    message->pane = (uint32_t)pane;

    if (kind == KMX_IMAGE_KIND_REF) {
        message->has_data = false;
        return offset == wire_size ? KMX_OK : KMX_ERR_PROTOCOL;
    }
    if (kind != KMX_IMAGE_KIND_DATA) return KMX_ERR_PROTOCOL;
    {
        uint64_t size;
        result = get_varint(bytes, wire_size, &offset, &size);
        if (result != KMX_OK) return result;
        if (size > KMX_MESSAGE_MAX) return KMX_ERR_LIMIT;
        if (wire_size - offset != size) return KMX_ERR_PROTOCOL;
        message->has_data = true;
        message->data = bytes + offset;
        message->size = (size_t)size;
    }
    return KMX_OK;
}
