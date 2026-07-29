/* Generate a fuzzing corpus of real messages.
 *
 * Starting a fuzzer from nothing means it spends its budget rediscovering the
 * magic numbers and varint framing before it reaches any interesting code.
 * Seeding it with genuine messages - and with each one truncated and
 * bit-flipped - starts it at the edges instead.
 *
 * Measured, because the size of that effect was worth knowing rather than
 * assuming.  At a ten-second budget the corpus is worth 817 coverage points
 * against 700 from cold.  At sixty seconds the two converge - 823 against 817 -
 * because these decoders are small enough that libFuzzer finds the framing on
 * its own given a little time.
 *
 * So this earns its place in short runs: the default `make fuzz`, and CI, where
 * the budget is measured in seconds.  For a long soak it makes almost no
 * difference, and claiming otherwise would be overselling it.
 *
 * Each file carries the one-byte decoder selector LLVMFuzzerTestOneInput
 * dispatches on, so the corpus works with the target as it is.
 *
 *   seed-corpus DIRECTORY
 */
#define _GNU_SOURCE

#include "kilix_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *directory;
static int written;

static void
emit(unsigned char selector, const void *data, size_t size, const char *label) {
    char path[512];
    FILE *file;
    snprintf(path, sizeof path, "%s/%s-%03d", directory, label, written++);
    file = fopen(path, "wb");
    if (!file) return;
    fwrite(&selector, 1, 1, file);
    if (size) fwrite(data, 1, size, file);
    fclose(file);
}

/* The whole message, plus the shapes a fuzzer would otherwise have to invent:
 * every message is interesting truncated, and interesting with one byte
 * wrong. */
static void
emit_family(unsigned char selector, const void *data, size_t size, const char *label) {
    unsigned char *copy;
    emit(selector, data, size, label);
    if (size > 4) emit(selector, data, size / 2, label);
    if (size > 1) emit(selector, data, size - 1, label);
    copy = malloc(size ? size : 1);
    if (!copy) return;
    memcpy(copy, data, size);
    if (size > 6) {
        copy[size / 3] ^= 0xff;
        emit(selector, copy, size, label);
    }
    free(copy);
}

int
main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: seed-corpus DIRECTORY\n");
        return 2;
    }
    directory = argv[1];
    mkdir(directory, 0700);

    /* Cell plane: an empty screen, a screen with content, and a diff. */
    {
        kmx_term *term = NULL;
        kmx_grid first;
        kmx_grid second;
        kmx_buffer message;
        memset(&first, 0, sizeof first);
        memset(&second, 0, sizeof second);
        if (kmx_term_create(&term, 24, 80) == KMX_OK) {
            kmx_term_snapshot(term, &first);
            kmx_buffer_init(&message);
            kmx_cells_encode(NULL, &first, &message);
            emit_family(0, message.data, message.size, "cells-blank");
            kmx_buffer_free(&message);

            kmx_term_feed(term, "\033[1;31mhello\033[0m world\r\nsecond", 30);
            kmx_term_snapshot(term, &second);
            kmx_buffer_init(&message);
            kmx_cells_encode(NULL, &second, &message);
            emit_family(0, message.data, message.size, "cells-full");
            kmx_buffer_free(&message);

            kmx_buffer_init(&message);
            kmx_cells_encode(&first, &second, &message);
            emit_family(0, message.data, message.size, "cells-diff");
            kmx_buffer_free(&message);

            kmx_grid_free(&first);
            kmx_grid_free(&second);
            kmx_term_free(term);
        }
    }

    /* Layout: one pane and several, since the pane loop is the interesting
     * part. */
    {
        kmx_layout layout;
        kmx_buffer message;
        size_t panes;
        for (panes = 1; panes <= 4; panes++) {
            kmx_layout_init(&layout, 24, 100);
            if (kmx_layout_arrange(&layout, panes, false) != KMX_OK) continue;
            snprintf(layout.panes[0].title, KMX_TITLE_MAX, "a pane title");
            kmx_buffer_init(&message);
            kmx_layout_encode(&layout, &message);
            emit_family(1, message.data, message.size, "layout");
            kmx_buffer_free(&message);
        }
    }

    /* Images: a transfer and a reference, which take different paths. */
    {
        kmx_buffer message;
        kmx_image_key key = kmx_image_key_of("Gi=1,a=T;PIXELS", 15);
        kmx_buffer_init(&message);
        kmx_image_encode(0, &key, "Gi=1,a=T;PIXELS", 15, &message);
        emit_family(2, message.data, message.size, "image-data");
        kmx_buffer_free(&message);
        kmx_buffer_init(&message);
        kmx_image_encode(0, &key, NULL, 0, &message);
        emit_family(2, message.data, message.size, "image-ref");
        kmx_buffer_free(&message);
    }

    /* Framing: one message and several back to back, because the boundary
     * between them is what a framer gets wrong. */
    {
        kmx_buffer stream;
        kmx_buffer_init(&stream);
        kmx_frame_encode(KMX_MSG_HELLO, "\0\30\0\120\0", 5, &stream);
        emit_family(3, stream.data, stream.size, "frame-one");
        kmx_frame_encode(KMX_MSG_INPUT, "typed", 5, &stream);
        kmx_frame_encode(KMX_MSG_EXIT, NULL, 0, &stream);
        emit_family(3, stream.data, stream.size, "frame-many");
        kmx_buffer_free(&stream);
    }

    /* Compression: something that compresses and something that does not, so
     * both codec branches are reachable from the corpus. */
    {
        kmx_buffer message;
        static unsigned char flat[4096];
        static unsigned char noisy[512];
        size_t index;
        uint64_t state = 0x9e3779b97f4a7c15ull;
        memset(flat, 'A', sizeof flat);
        for (index = 0; index < sizeof noisy; index++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            noisy[index] = (unsigned char)(state >> 32);
        }
        kmx_buffer_init(&message);
        kmx_compress(flat, sizeof flat, &message);
        emit_family(4, message.data, message.size, "zstd-flat");
        kmx_buffer_free(&message);
        kmx_buffer_init(&message);
        kmx_compress(noisy, sizeof noisy, &message);
        emit_family(4, message.data, message.size, "zstd-noise");
        kmx_buffer_free(&message);
    }

    /* Motion: a keyframe and a delta. */
    {
        kmx_motion *motion = NULL;
        static unsigned char frame[64 * 48 * 3];
        kmx_buffer message;
        bool produced = false;
        size_t index;
        for (index = 0; index < sizeof frame; index++) {
            frame[index] = (unsigned char)(index * 31);
        }
        if (kmx_motion_create(&motion, 0) == KMX_OK) {
            kmx_buffer_init(&message);
            if (kmx_motion_offer(motion, frame, 64, 48, 0, &message, &produced, NULL) == KMX_OK
                && produced) {
                emit_family(5, message.data, message.size, "motion-key");
            }
            kmx_buffer_free(&message);
            memset(frame + 3 * 64 * 3, 0x5a, 64 * 3);
            kmx_buffer_init(&message);
            produced = false;
            if (kmx_motion_offer(motion, frame, 64, 48, 100, &message, &produced, NULL) == KMX_OK
                && produced) {
                emit_family(5, message.data, message.size, "motion-delta");
            }
            kmx_buffer_free(&message);
            kmx_motion_free(motion);
        }
    }

    /* Audio. */
    {
        kmx_audio *audio = NULL;
        static unsigned char pcm[3840];
        kmx_buffer message;
        bool produced = false;
        if (kmx_audio_create(&audio, 48000, 2, 0) == KMX_OK) {
            kmx_buffer_init(&message);
            if (kmx_audio_offer(audio, pcm, sizeof pcm, 1000, &message, &produced, NULL) == KMX_OK
                && produced) {
                emit_family(6, message.data, message.size, "audio");
            }
            kmx_buffer_free(&message);
            kmx_audio_free(audio);
        }
    }

    /* The receiver, which is the cell plane behind a sequence number and the
     * compression framing - the path a real peer actually drives. */
    {
        kmx_sync *sync = NULL;
        kmx_buffer message;
        kmx_sync_info info;
        bool produced = false;
        if (kmx_sync_create(&sync, 24, 80) == KMX_OK) {
            kmx_sync_feed(sync, "receiver corpus", 15);
            kmx_buffer_init(&message);
            if (kmx_sync_poll(sync, 1000, &message, &produced, &info) == KMX_OK && produced) {
                emit_family(7, message.data, message.size, "receiver");
            }
            kmx_buffer_free(&message);
            kmx_sync_free(sync);
        }
    }

    printf("wrote %d corpus files to %s\n", written, directory);
    return 0;
}
