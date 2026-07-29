/* libFuzzer target for every decoder a remote peer can reach.
 *
 * These are the parsers fed directly by the other end of the connection, so
 * "it passed the tests" is not a sufficient argument for any of them.  Each
 * may reject whatever it likes; none may crash, read out of bounds, or
 * allocate without limit.
 *
 * The first input byte selects a decoder, so one corpus exercises all of them
 * and libFuzzer's coverage feedback can steer into each. */
#include "kilix_mux.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fuzz_cells(const uint8_t *data, size_t size) {
    kmx_grid grid;
    if (kmx_grid_init(&grid, 24, 80) != KMX_OK) return;
    (void)kmx_cells_apply(&grid, data, size);
    kmx_grid_free(&grid);
}

static void
fuzz_layout(const uint8_t *data, size_t size) {
    kmx_layout layout;
    if (kmx_layout_apply(&layout, data, size) != KMX_OK) return;
    /* A layout that decoded must also be safe to composite against, since
     * that is what the client does with it next. */
    {
        kmx_grid screen;
        memset(&screen, 0, sizeof screen);
        (void)kmx_layout_composite(&layout, NULL, 0, &screen);
        kmx_grid_free(&screen);
    }
}

static void
fuzz_image(const uint8_t *data, size_t size) {
    kmx_image_message message;
    kmx_image_cache *cache = NULL;
    if (kmx_image_decode(data, size, &message) != KMX_OK) return;
    if (kmx_image_cache_create(&cache, 8, 65536) != KMX_OK) return;
    if (message.has_data) {
        (void)kmx_image_cache_put(cache, &message.key, message.data, message.size);
        (void)kmx_image_cache_get(cache, &message.key, NULL);
    }
    kmx_image_cache_free(cache);
}

static void
fuzz_framer(const uint8_t *data, size_t size) {
    kmx_framer framer;
    size_t index;
    kmx_framer_init(&framer);
    /* Pushed in small pieces, because a socket delivers whatever it likes and
     * the boundary between messages is exactly what a framer can get wrong. */
    for (index = 0; index < size; index += 7) {
        size_t chunk = size - index < 7 ? size - index : 7;
        if (kmx_framer_push(&framer, data + index, chunk) != KMX_OK) break;
        while (true) {
            bool ready = false;
            kmx_message_type type;
            const unsigned char *payload;
            size_t payload_size;
            if (kmx_framer_next(
                    &framer, &ready, &type, &payload, &payload_size) != KMX_OK) {
                goto done;
            }
            if (!ready) break;
            kmx_framer_consume(&framer);
        }
    }
done:
    kmx_framer_free(&framer);
}

static void
fuzz_decompress(const uint8_t *data, size_t size) {
    kmx_buffer out;
    kmx_buffer_init(&out);
    (void)kmx_decompress(data, size, &out);
    kmx_buffer_free(&out);
}

static void
fuzz_motion(const uint8_t *data, size_t size) {
    kmx_motion_sink *sink = NULL;
    if (kmx_motion_sink_create(&sink) != KMX_OK) return;
    (void)kmx_motion_sink_apply(sink, data, size);
    kmx_motion_sink_free(sink);
}

static void
fuzz_audio(const uint8_t *data, size_t size) {
    kmx_audio_sink *sink = NULL;
    if (kmx_audio_sink_create(&sink) != KMX_OK) return;
    (void)kmx_audio_sink_apply(sink, data, size);
    kmx_audio_sink_free(sink);
}

static void
fuzz_receiver(const uint8_t *data, size_t size) {
    kmx_receiver *receiver = NULL;
    uint64_t sequence = 0;
    if (kmx_receiver_create(&receiver, 24, 80) != KMX_OK) return;
    (void)kmx_receiver_apply(receiver, data, size, &sequence);
    kmx_receiver_free(receiver);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) return 0;
    switch (data[0] % 8) {
        case 0: fuzz_cells(data + 1, size - 1); break;
        case 1: fuzz_layout(data + 1, size - 1); break;
        case 2: fuzz_image(data + 1, size - 1); break;
        case 3: fuzz_framer(data + 1, size - 1); break;
        case 4: fuzz_decompress(data + 1, size - 1); break;
        case 5: fuzz_motion(data + 1, size - 1); break;
        case 6: fuzz_audio(data + 1, size - 1); break;
        default: fuzz_receiver(data + 1, size - 1); break;
    }
    return 0;
}
