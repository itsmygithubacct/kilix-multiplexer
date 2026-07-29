/* The audio plane.
 *
 * Small next to video and far less forgiving.  A dropped video frame goes
 * unnoticed; a gap in sound does not.  So this plane takes its allowance
 * first and effectively fixed, rate adaptation belongs to the motion plane,
 * and when the allowance really is spent a block is dropped rather than
 * buffered - audio must never be able to stall the scheduler waiting for room.
 *
 * Blocks carry a presentation timestamp against one session clock.  That is
 * what keeps sound and picture together without either waiting on the other:
 * when video falls behind, video degrades, and audio does not slow down to
 * match.
 *
 * The payload here is PCM over zstd.  As with the motion plane, that keeps
 * this free of an encoder dependency; AAC or Opus can be added as another
 * codec choice without changing the plane's shape. */
#include "kilix_mux.h"

#include <stdlib.h>
#include <string.h>

#define KMX_AUDIO_MAGIC 0x4b4d4101u /* "KMA\1" */

/* One block is bounded so a corrupt or hostile length cannot drive an
 * arbitrary allocation.  A second of 48 kHz stereo 16-bit is under 200 kB, so
 * this is generous for any real block. */
#define KMX_AUDIO_BLOCK_MAX (1024u * 1024u)

struct kmx_audio {
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t budget;
    uint64_t window_start;
    size_t window_bytes;
    size_t dropped;
};

struct kmx_audio_sink {
    unsigned char *pcm;
    size_t bytes;
    size_t capacity;
    uint32_t sample_rate;
    uint8_t channels;
    uint64_t timestamp;
    uint64_t expected_next;
    uint64_t gap_millis;
    bool primed;
};

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

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
} reader;

static kmx_result
get_varint(reader *in, uint64_t *value) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        unsigned char byte;
        if (in->offset >= in->size) return KMX_ERR_TRUNCATED;
        byte = in->data[in->offset++];
        if (shift > 63) return KMX_ERR_PROTOCOL;
        result |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) break;
        shift += 7;
    }
    *value = result;
    return KMX_OK;
}

kmx_result
kmx_audio_create(
    kmx_audio **out,
    uint32_t sample_rate,
    uint8_t channels,
    uint32_t bytes_per_second
) {
    kmx_audio *audio;
    if (!out || !sample_rate || !channels) return KMX_ERR_INVALID;
    if (sample_rate > 384000u || channels > 8u) return KMX_ERR_INVALID;
    audio = calloc(1, sizeof *audio);
    if (!audio) return KMX_ERR_MEMORY;
    audio->sample_rate = sample_rate;
    audio->channels = channels;
    audio->budget = bytes_per_second;
    *out = audio;
    return KMX_OK;
}

void
kmx_audio_free(kmx_audio *audio) {
    free(audio);
}

size_t
kmx_audio_dropped(const kmx_audio *audio) {
    return audio ? audio->dropped : 0;
}

kmx_result
kmx_audio_offer(
    kmx_audio *audio,
    const void *pcm,
    size_t bytes,
    uint64_t timestamp_millis,
    kmx_buffer *out,
    bool *produced,
    kmx_audio_info *info
) {
    kmx_buffer body;
    kmx_result result;

    if (!audio || !pcm || !out || !produced) return KMX_ERR_INVALID;
    if (!bytes || bytes > KMX_AUDIO_BLOCK_MAX) return KMX_ERR_INVALID;
    *produced = false;

    if (audio->budget) {
        if (timestamp_millis - audio->window_start >= 1000) {
            audio->window_start = timestamp_millis;
            audio->window_bytes = 0;
        }
        if (audio->window_bytes >= audio->budget) {
            /* Dropped, not held.  Holding it would delay everything behind it
             * and still arrive too late to play. */
            audio->dropped++;
            return KMX_OK;
        }
    }

    kmx_buffer_init(&body);
    result = put_varint(&body, audio->sample_rate);
    if (result == KMX_OK) result = kmx_buffer_append(&body, &audio->channels, 1);
    if (result == KMX_OK) result = put_varint(&body, timestamp_millis);
    if (result == KMX_OK) result = put_varint(&body, bytes);
    if (result == KMX_OK) result = kmx_buffer_append(&body, pcm, bytes);
    if (result != KMX_OK) {
        kmx_buffer_free(&body);
        return result;
    }
    {
        unsigned char header[4];
        header[0] = (unsigned char)(KMX_AUDIO_MAGIC >> 24);
        header[1] = (unsigned char)(KMX_AUDIO_MAGIC >> 16);
        header[2] = (unsigned char)(KMX_AUDIO_MAGIC >> 8);
        header[3] = (unsigned char)KMX_AUDIO_MAGIC;
        result = kmx_buffer_append(out, header, sizeof header);
    }
    if (result == KMX_OK) result = kmx_compress(body.data, body.size, out);
    kmx_buffer_free(&body);
    if (result != KMX_OK) return result;

    if (audio->budget) audio->window_bytes += out->size;
    *produced = true;
    if (info) {
        info->timestamp_millis = timestamp_millis;
        info->wire_bytes = out->size;
        info->pcm_bytes = bytes;
    }
    return KMX_OK;
}

/* ---- sink ------------------------------------------------------------- */

kmx_result
kmx_audio_sink_create(kmx_audio_sink **out) {
    kmx_audio_sink *sink;
    if (!out) return KMX_ERR_INVALID;
    sink = calloc(1, sizeof *sink);
    if (!sink) return KMX_ERR_MEMORY;
    *out = sink;
    return KMX_OK;
}

void
kmx_audio_sink_free(kmx_audio_sink *sink) {
    if (!sink) return;
    free(sink->pcm);
    free(sink);
}

const unsigned char *
kmx_audio_sink_pcm(
    const kmx_audio_sink *sink,
    size_t *bytes,
    uint64_t *timestamp_millis
) {
    if (!sink) return NULL;
    if (bytes) *bytes = sink->bytes;
    if (timestamp_millis) *timestamp_millis = sink->timestamp;
    return sink->pcm;
}

uint32_t
kmx_audio_sink_sample_rate(const kmx_audio_sink *sink) {
    return sink ? sink->sample_rate : 0;
}

uint8_t
kmx_audio_sink_channels(const kmx_audio_sink *sink) {
    return sink ? sink->channels : 0;
}

uint64_t
kmx_audio_sink_gap_millis(const kmx_audio_sink *sink) {
    return sink ? sink->gap_millis : 0;
}

kmx_result
kmx_audio_sink_apply(kmx_audio_sink *sink, const void *data, size_t size) {
    const unsigned char *bytes = data;
    kmx_buffer plain;
    reader in;
    uint64_t sample_rate;
    uint64_t timestamp;
    uint64_t block;
    unsigned char channels;
    kmx_result result;

    if (!sink || (!data && size)) return KMX_ERR_INVALID;
    if (size < 4) return KMX_ERR_TRUNCATED;
    if (((uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
         (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3]) != KMX_AUDIO_MAGIC) {
        return KMX_ERR_PROTOCOL;
    }

    kmx_buffer_init(&plain);
    result = kmx_decompress(bytes + 4, size - 4, &plain);
    if (result != KMX_OK) {
        kmx_buffer_free(&plain);
        return result;
    }
    in.data = plain.data;
    in.size = plain.size;
    in.offset = 0;

    result = get_varint(&in, &sample_rate);
    if (result == KMX_OK && in.offset >= in.size) result = KMX_ERR_TRUNCATED;
    if (result != KMX_OK) {
        kmx_buffer_free(&plain);
        return result;
    }
    channels = in.data[in.offset++];
    result = get_varint(&in, &timestamp);
    if (result == KMX_OK) result = get_varint(&in, &block);
    if (result == KMX_OK &&
        (!sample_rate || sample_rate > 384000u || !channels || channels > 8u ||
         !block || block > KMX_AUDIO_BLOCK_MAX)) {
        result = KMX_ERR_PROTOCOL;
    }
    if (result == KMX_OK && in.size - in.offset != block) result = KMX_ERR_PROTOCOL;
    if (result != KMX_OK) {
        kmx_buffer_free(&plain);
        return result;
    }

    if (block > sink->capacity) {
        unsigned char *replacement = realloc(sink->pcm, (size_t)block);
        if (!replacement) {
            kmx_buffer_free(&plain);
            return KMX_ERR_MEMORY;
        }
        sink->pcm = replacement;
        sink->capacity = (size_t)block;
    }
    memcpy(sink->pcm, in.data + in.offset, (size_t)block);
    sink->bytes = (size_t)block;
    sink->sample_rate = (uint32_t)sample_rate;
    sink->channels = channels;
    sink->timestamp = timestamp;

    /* A block that starts later than the previous one ended is a gap.  It is
     * reported rather than hidden, because a consumer conceals a gap it knows
     * about far better than one that surprises it. */
    {
        uint64_t frame_bytes = (uint64_t)channels * 2u;
        uint64_t frames = frame_bytes ? block / frame_bytes : 0;
        uint64_t duration = sample_rate ? (frames * 1000u) / sample_rate : 0;
        if (sink->primed && timestamp > sink->expected_next) {
            sink->gap_millis += timestamp - sink->expected_next;
        }
        sink->expected_next = timestamp + duration;
        sink->primed = true;
    }
    kmx_buffer_free(&plain);
    return KMX_OK;
}
