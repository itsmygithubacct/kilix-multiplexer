/* The motion plane.
 *
 * A pane whose content is pixels: a browser, a GUI app, a desktop.  Every
 * pixel protocol has to work out which parts of a screen are moving; this one
 * is told, because Kilix launched the pane and knows what it is.  What is left
 * is the easy half - send what changed, within a budget.
 *
 * This is the only plane allowed to drop.  A frame that does not fit the rate
 * allowance is discarded rather than queued, because the next frame supersedes
 * it: the same reasoning as the cell plane's diff-against-last-ack, applied to
 * pixels.  Text is never treated this way.
 *
 * The codec is lossless rectangles over zstd.  That is a good default for
 * screen content, and it keeps this plane free of an encoder dependency; a
 * lossy or inter-frame codec can be added later as another choice without
 * changing the plane's shape. */
#include "kilix_mux.h"

#include <stdlib.h>
#include <string.h>

#define KMX_MOTION_MAGIC 0x4b4d5601u /* "KMV\1" */
#define KMX_MOTION_BYTES 3           /* RGB */

/* Rectangles are found by scanning bands of rows, which is cheap and matches
 * how screen content actually changes.  A frame that changed in more places
 * than this is sent whole: past a point, describing the damage costs more than
 * the pixels. */
#define KMX_MOTION_BAND 16

struct kmx_motion {
    unsigned char *previous;
    int width;
    int height;
    bool have_previous;
    uint32_t budget;         /* bytes per second; 0 means no ceiling */
    uint64_t window_start;
    size_t window_bytes;
    size_t dropped;
};

struct kmx_motion_sink {
    unsigned char *pixels;
    int width;
    int height;
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
kmx_motion_create(kmx_motion **out, uint32_t bytes_per_second) {
    kmx_motion *motion;
    if (!out) return KMX_ERR_INVALID;
    motion = calloc(1, sizeof *motion);
    if (!motion) return KMX_ERR_MEMORY;
    motion->budget = bytes_per_second;
    *out = motion;
    return KMX_OK;
}

void
kmx_motion_free(kmx_motion *motion) {
    if (!motion) return;
    free(motion->previous);
    free(motion);
}

void
kmx_motion_invalidate(kmx_motion *motion) {
    if (motion) motion->have_previous = false;
}

size_t
kmx_motion_dropped(const kmx_motion *motion) {
    return motion ? motion->dropped : 0;
}

/* Dimensions bound an allocation, so they are checked at the width they
 * arrived at.
 *
 * The decoder reads them as 64-bit varints, and an earlier version validated
 * `(int)width` while computing the frame size from the full value - so a width
 * of 0x100000001 truncated to 1, passed the check, and then asked for twenty
 * petabytes.  Found by a long fuzz soak, having survived every short run.
 * Hence one validator over uint64_t, used before anything is narrowed. */
static bool
valid_dimensions(uint64_t width, uint64_t height) {
    if (!width || !height) return false;
    if (width > 8192u || height > 8192u) return false;
    return width * height * KMX_MOTION_BYTES <= 64u * 1024u * 1024u;
}

static bool
valid_frame(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    return valid_dimensions((uint64_t)width, (uint64_t)height);
}

static bool
band_changed(
    const unsigned char *previous,
    const unsigned char *current,
    int width,
    int start,
    int end
) {
    size_t offset = (size_t)start * (size_t)width * KMX_MOTION_BYTES;
    size_t length = (size_t)(end - start) * (size_t)width * KMX_MOTION_BYTES;
    return memcmp(previous + offset, current + offset, length) != 0;
}

kmx_result
kmx_motion_offer(
    kmx_motion *motion,
    const void *rgb,
    int width,
    int height,
    uint64_t now_millis,
    kmx_buffer *out,
    bool *produced,
    kmx_motion_info *info
) {
    const unsigned char *pixels = rgb;
    kmx_buffer body;
    size_t frame_bytes;
    bool keyframe;
    size_t rects = 0;
    kmx_result result;

    if (!motion || !rgb || !out || !produced) return KMX_ERR_INVALID;
    if (!valid_frame(width, height)) return KMX_ERR_INVALID;
    *produced = false;
    frame_bytes = (size_t)width * (size_t)height * KMX_MOTION_BYTES;

    keyframe = !motion->have_previous ||
        motion->width != width || motion->height != height;

    if (!keyframe && memcmp(motion->previous, pixels, frame_bytes) == 0) {
        /* A pane that is not moving costs nothing, exactly like an idle
         * terminal. */
        return KMX_OK;
    }

    /* The rate allowance is a sliding second.  Spending it means dropping this
     * frame, not queuing it: by the time there is room, a newer frame will
     * have arrived and this one would be stale. */
    if (motion->budget) {
        if (now_millis - motion->window_start >= 1000) {
            motion->window_start = now_millis;
            motion->window_bytes = 0;
        }
        if (motion->window_bytes >= motion->budget) {
            motion->dropped++;
            return KMX_OK;
        }
    }

    kmx_buffer_init(&body);
    result = put_varint(&body, (uint64_t)width);
    if (result == KMX_OK) result = put_varint(&body, (uint64_t)height);

    if (keyframe) {
        unsigned char flags = 1u;
        if (result == KMX_OK) result = kmx_buffer_append(&body, &flags, 1);
        if (result == KMX_OK) result = kmx_buffer_append(&body, pixels, frame_bytes);
        rects = 1;
    } else {
        unsigned char flags = 0u;
        kmx_buffer bands;
        uint64_t count = 0;
        int row = 0;
        kmx_buffer_init(&bands);
        while (row < height && result == KMX_OK) {
            int end = row + KMX_MOTION_BAND;
            if (end > height) end = height;
            if (band_changed(motion->previous, pixels, width, row, end)) {
                size_t offset = (size_t)row * (size_t)width * KMX_MOTION_BYTES;
                size_t length = (size_t)(end - row) * (size_t)width * KMX_MOTION_BYTES;
                result = put_varint(&bands, (uint64_t)row);
                if (result == KMX_OK) result = put_varint(&bands, (uint64_t)(end - row));
                if (result == KMX_OK) {
                    result = kmx_buffer_append(&bands, pixels + offset, length);
                }
                count++;
            }
            row = end;
        }
        rects = (size_t)count;
        if (result == KMX_OK) result = kmx_buffer_append(&body, &flags, 1);
        if (result == KMX_OK) result = put_varint(&body, count);
        if (result == KMX_OK) result = kmx_buffer_append(&body, bands.data, bands.size);
        kmx_buffer_free(&bands);
    }
    if (result != KMX_OK) {
        kmx_buffer_free(&body);
        return result;
    }

    {
        unsigned char header[4];
        header[0] = (unsigned char)(KMX_MOTION_MAGIC >> 24);
        header[1] = (unsigned char)(KMX_MOTION_MAGIC >> 16);
        header[2] = (unsigned char)(KMX_MOTION_MAGIC >> 8);
        header[3] = (unsigned char)KMX_MOTION_MAGIC;
        result = kmx_buffer_append(out, header, sizeof header);
    }
    if (result == KMX_OK) result = kmx_compress(body.data, body.size, out);
    kmx_buffer_free(&body);
    if (result != KMX_OK) return result;

    if (motion->width != width || motion->height != height) {
        unsigned char *replacement = realloc(motion->previous, frame_bytes);
        if (!replacement) return KMX_ERR_MEMORY;
        motion->previous = replacement;
        motion->width = width;
        motion->height = height;
    }
    memcpy(motion->previous, pixels, frame_bytes);
    motion->have_previous = true;
    if (motion->budget) motion->window_bytes += out->size;
    *produced = true;
    if (info) {
        info->keyframe = keyframe;
        info->rects = rects;
        info->wire_bytes = out->size;
        info->pixel_bytes = frame_bytes;
    }
    return KMX_OK;
}

/* ---- sink ------------------------------------------------------------- */

kmx_result
kmx_motion_sink_create(kmx_motion_sink **out) {
    kmx_motion_sink *sink;
    if (!out) return KMX_ERR_INVALID;
    sink = calloc(1, sizeof *sink);
    if (!sink) return KMX_ERR_MEMORY;
    *out = sink;
    return KMX_OK;
}

void
kmx_motion_sink_free(kmx_motion_sink *sink) {
    if (!sink) return;
    free(sink->pixels);
    free(sink);
}

const unsigned char *
kmx_motion_sink_pixels(const kmx_motion_sink *sink, int *width, int *height) {
    if (!sink) return NULL;
    if (width) *width = sink->width;
    if (height) *height = sink->height;
    return sink->pixels;
}

kmx_result
kmx_motion_sink_apply(kmx_motion_sink *sink, const void *data, size_t size) {
    const unsigned char *bytes = data;
    kmx_buffer plain;
    reader in;
    uint64_t width;
    uint64_t height;
    unsigned char flags;
    size_t frame_bytes;
    kmx_result result;

    if (!sink || (!data && size)) return KMX_ERR_INVALID;
    if (size < 4) return KMX_ERR_TRUNCATED;
    if (((uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
         (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3]) != KMX_MOTION_MAGIC) {
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

    result = get_varint(&in, &width);
    if (result == KMX_OK) result = get_varint(&in, &height);
    /* Checked before narrowing, not after. */
    if (result == KMX_OK && !valid_dimensions(width, height)) {
        result = KMX_ERR_PROTOCOL;
    }
    if (result == KMX_OK && in.offset >= in.size) result = KMX_ERR_TRUNCATED;
    if (result != KMX_OK) {
        kmx_buffer_free(&plain);
        return result;
    }
    flags = in.data[in.offset++];
    frame_bytes = (size_t)width * (size_t)height * KMX_MOTION_BYTES;

    if (sink->width != (int)width || sink->height != (int)height) {
        unsigned char *replacement;
        /* A delta cannot be the first thing seen for a size: there is nothing
         * to apply it to. */
        if (!(flags & 1u)) {
            kmx_buffer_free(&plain);
            return KMX_ERR_PROTOCOL;
        }
        replacement = calloc(frame_bytes ? frame_bytes : 1, 1);
        if (!replacement) {
            kmx_buffer_free(&plain);
            return KMX_ERR_MEMORY;
        }
        free(sink->pixels);
        sink->pixels = replacement;
        sink->width = (int)width;
        sink->height = (int)height;
    }

    if (flags & 1u) {
        if (in.size - in.offset != frame_bytes) {
            kmx_buffer_free(&plain);
            return KMX_ERR_PROTOCOL;
        }
        memcpy(sink->pixels, in.data + in.offset, frame_bytes);
        kmx_buffer_free(&plain);
        return KMX_OK;
    }
    {
        uint64_t count;
        uint64_t band;
        result = get_varint(&in, &count);
        if (result == KMX_OK && count > (uint64_t)height) result = KMX_ERR_PROTOCOL;
        for (band = 0; band < count && result == KMX_OK; band++) {
            uint64_t row;
            uint64_t rows;
            size_t offset;
            size_t length;
            result = get_varint(&in, &row);
            if (result == KMX_OK) result = get_varint(&in, &rows);
            if (result != KMX_OK) break;
            if (!rows || row + rows > height) {
                result = KMX_ERR_PROTOCOL;
                break;
            }
            offset = (size_t)row * (size_t)width * KMX_MOTION_BYTES;
            length = (size_t)rows * (size_t)width * KMX_MOTION_BYTES;
            if (in.size - in.offset < length) {
                result = KMX_ERR_TRUNCATED;
                break;
            }
            memcpy(sink->pixels + offset, in.data + in.offset, length);
            in.offset += length;
        }
        if (result == KMX_OK && in.offset != in.size) result = KMX_ERR_PROTOCOL;
    }
    kmx_buffer_free(&plain);
    return result;
}
