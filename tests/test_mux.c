#define _GNU_SOURCE

#include "kilix_mux.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *current_test = "(startup)";

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: [%s] check failed: %s\n", \
                __FILE__, __LINE__, current_test, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(test) do { current_test = #test; test(); } while (0)

/* Deterministic, so a failure is reproducible without a recorded seed. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint32_t
next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static void
reset_random(void) {
    rng_state = 0x9e3779b97f4a7c15ull;
}

/* ---- the convergence harness ------------------------------------------
 *
 * A sender terminal, the state the receiver is believed to hold, and the
 * receiver's own grid.  Every step diffs against the believed state, applies
 * the result to the receiver, and asserts the two agree exactly.  That is the
 * property the whole cell plane rests on. */
typedef struct {
    kmx_term *term;
    kmx_grid sent;     /* what the receiver is believed to hold */
    kmx_grid current;  /* what the sender's terminal shows now */
    kmx_grid received; /* what the receiver actually holds */
    size_t bytes_on_wire;
    bool primed;
} harness;

static void
harness_init(harness *h, int rows, int cols) {
    memset(h, 0, sizeof *h);
    CHECK(kmx_term_create(&h->term, rows, cols) == KMX_OK);
    CHECK(kmx_grid_init(&h->sent, rows, cols) == KMX_OK);
    CHECK(kmx_grid_init(&h->current, rows, cols) == KMX_OK);
    CHECK(kmx_grid_init(&h->received, rows, cols) == KMX_OK);
}

static void
harness_free(harness *h) {
    kmx_term_free(h->term);
    kmx_grid_free(&h->sent);
    kmx_grid_free(&h->current);
    kmx_grid_free(&h->received);
}

/* Feed bytes, then synchronise the receiver and assert convergence. */
static size_t
harness_step(harness *h, const char *text) {
    kmx_buffer message;
    size_t size;
    CHECK(kmx_term_feed(h->term, text, strlen(text)) == KMX_OK);
    CHECK(kmx_term_snapshot(h->term, &h->current) == KMX_OK);
    kmx_buffer_init(&message);
    CHECK(kmx_cells_encode(h->primed ? &h->sent : NULL, &h->current, &message) == KMX_OK);
    CHECK(kmx_cells_apply(&h->received, message.data, message.size) == KMX_OK);
    CHECK(kmx_grid_equal(&h->received, &h->current));
    CHECK(kmx_grid_copy(&h->sent, &h->current) == KMX_OK);
    h->primed = true;
    size = message.size;
    h->bytes_on_wire += size;
    kmx_buffer_free(&message);
    return size;
}

/* Feed bytes without synchronising, so the receiver falls behind. */
static void
harness_feed_only(harness *h, const char *text) {
    CHECK(kmx_term_feed(h->term, text, strlen(text)) == KMX_OK);
}

static void
test_grid_basics(void) {
    kmx_grid grid;
    kmx_grid other;
    CHECK(kmx_grid_init(&grid, 4, 8) == KMX_OK);
    CHECK(kmx_grid_init(&other, 4, 8) == KMX_OK);
    CHECK(kmx_grid_equal(&grid, &other));
    kmx_grid_cell(&grid, 1, 2)->chars[0] = 'x';
    CHECK(!kmx_grid_equal(&grid, &other));
    CHECK(kmx_grid_copy(&other, &grid) == KMX_OK);
    CHECK(kmx_grid_equal(&grid, &other));
    CHECK(kmx_grid_cell(&grid, 4, 0) == NULL);
    CHECK(kmx_grid_cell(&grid, 0, 8) == NULL);
    CHECK(kmx_grid_cell(&grid, -1, 0) == NULL);
    /* Dimensions are bounded so a corrupt message cannot ask for the address
     * space. */
    CHECK(kmx_grid_init(&other, 0, 8) == KMX_ERR_INVALID);
    CHECK(kmx_grid_init(&other, 100000, 100000) == KMX_ERR_INVALID);
    kmx_grid_free(&grid);
    kmx_grid_free(&other);
}

static void
test_roundtrip_text(void) {
    harness h;
    harness_init(&h, 24, 80);
    harness_step(&h, "hello world");
    harness_step(&h, "\r\nsecond line");
    harness_step(&h, "\r\n\033[1;31mred bold\033[0m plain");
    harness_step(&h, "\033[H\033[2Jcleared");
    harness_free(&h);
}

static void
test_roundtrip_attributes(void) {
    harness h;
    harness_init(&h, 10, 40);
    harness_step(&h, "\033[1mbold\033[0m ");
    harness_step(&h, "\033[3mitalic\033[0m ");
    harness_step(&h, "\033[4munderline\033[0m ");
    harness_step(&h, "\033[7mreverse\033[0m ");
    harness_step(&h, "\033[9mstrike\033[0m ");
    harness_step(&h, "\033[38;2;10;20;30mtruecolour\033[0m ");
    harness_step(&h, "\033[48;5;123mindexed\033[0m ");
    harness_step(&h, "\033[5mblink\033[0m ");
    harness_free(&h);
}

static void
test_roundtrip_wide_and_combining(void) {
    harness h;
    harness_init(&h, 8, 30);
    /* Wide characters occupy two columns; the second carries width 0. */
    harness_step(&h, "\xe4\xbd\xa0\xe5\xa5\xbd wide");
    /* A base character followed by a combining accent must survive as one
     * cell with two codepoints. */
    harness_step(&h, "\r\ne\xcc\x81 combining");
    harness_step(&h, "\r\n\xf0\x9f\x9a\x80 emoji");
    harness_free(&h);
}

static void
test_roundtrip_scrolling(void) {
    harness h;
    int index;
    char line[64];
    harness_init(&h, 6, 20);
    for (index = 0; index < 40; index++) {
        snprintf(line, sizeof line, "line %d\r\n", index);
        harness_step(&h, line);
    }
    harness_free(&h);
}

/* The randomised half: arbitrary text, cursor motion, colour changes and
 * erases, checked for exact convergence at every step. */
static void
test_roundtrip_randomised(void) {
    harness h;
    int step;
    reset_random();
    harness_init(&h, 12, 40);
    for (step = 0; step < 400; step++) {
        char text[128];
        switch (next_random() % 8) {
            case 0:
                snprintf(text, sizeof text, "\033[%u;%uH",
                         next_random() % 12 + 1, next_random() % 40 + 1);
                break;
            case 1:
                snprintf(text, sizeof text, "\033[%um", next_random() % 8);
                break;
            case 2:
                snprintf(text, sizeof text, "\033[3%um", next_random() % 8);
                break;
            case 3:
                snprintf(text, sizeof text, "\033[4%um", next_random() % 8);
                break;
            case 4:
                snprintf(text, sizeof text, "\033[%uK", next_random() % 3);
                break;
            case 5:
                snprintf(text, sizeof text, "\033[%uJ", next_random() % 3);
                break;
            case 6:
                snprintf(text, sizeof text, "\r\n");
                break;
            default: {
                size_t length = next_random() % 20 + 1;
                size_t index;
                for (index = 0; index < length; index++) {
                    text[index] = (char)('!' + (next_random() % 90));
                }
                text[length] = '\0';
                break;
            }
        }
        harness_step(&h, text);
    }
    harness_free(&h);
}

/* The property that makes a bad link cheap: a receiver that missed a great
 * deal is brought current by ONE message describing the screen it should now
 * have, not by replaying what it missed. */
static void
test_stale_receiver_catches_up_in_one_message(void) {
    harness h;
    kmx_buffer message;
    char line[64];
    int index;
    size_t catch_up_bytes;
    size_t skipped_bytes = 0;

    harness_init(&h, 10, 40);
    harness_step(&h, "starting point");

    /* Megabytes of scrollback the receiver never sees. */
    for (index = 0; index < 20000; index++) {
        snprintf(line, sizeof line, "noise %d some filler text here\r\n", index);
        skipped_bytes += strlen(line);
        harness_feed_only(&h, line);
    }
    harness_feed_only(&h, "FINAL STATE");
    CHECK(kmx_term_snapshot(h.term, &h.current) == KMX_OK);

    kmx_buffer_init(&message);
    CHECK(kmx_cells_encode(&h.sent, &h.current, &message) == KMX_OK);
    catch_up_bytes = message.size;
    CHECK(kmx_cells_apply(&h.received, message.data, message.size) == KMX_OK);
    CHECK(kmx_grid_equal(&h.received, &h.current));
    kmx_buffer_free(&message);

    /* Roughly six hundred kilobytes went past; catching up costs one screen. */
    CHECK(skipped_bytes > 500000);
    CHECK(catch_up_bytes < 8000);
    harness_free(&h);
}

/* A one-cell change must cost about one cell, not one screen. */
static void
test_small_change_is_a_small_message(void) {
    harness h;
    size_t full;
    size_t incremental;
    harness_init(&h, 24, 80);
    full = harness_step(&h, "\033[2J\033[HA screen with some content on it");
    incremental = harness_step(&h, "!");
    CHECK(incremental * 8 < full);
    harness_free(&h);
}

static void
test_resize(void) {
    harness h;
    harness_init(&h, 10, 40);
    harness_step(&h, "before resize");
    CHECK(kmx_term_resize(h.term, 20, 60) == KMX_OK);
    harness_step(&h, " after");
    CHECK(h.current.rows == 20);
    CHECK(h.current.cols == 60);
    CHECK(h.received.rows == 20);
    CHECK(h.received.cols == 60);
    CHECK(kmx_term_resize(h.term, 5, 15) == KMX_OK);
    harness_step(&h, " smaller");
    CHECK(h.received.rows == 5 && h.received.cols == 15);
    harness_free(&h);
}

/* Graphics escapes are captured whole, in order, and interleaved correctly
 * with the text around them. */
static void
test_graphics_captured_in_wire_order(void) {
    kmx_term *term;
    kmx_grid grid;
    const kmx_graphics_event *event;
    static const char stream[] =
        "before\033_Gi=1,a=T;FIRSTPAYLOAD\033\\middle\033_Gi=2,a=p;SECOND\033\\after";

    CHECK(kmx_term_create(&term, 10, 40) == KMX_OK);
    CHECK(kmx_grid_init(&grid, 10, 40) == KMX_OK);
    CHECK(kmx_term_feed(term, stream, sizeof stream - 1) == KMX_OK);
    CHECK(kmx_term_snapshot(term, &grid) == KMX_OK);

    CHECK(kmx_term_graphics_count(term) == 2);
    event = kmx_term_graphics_at(term, 0);
    CHECK(event != NULL);
    CHECK(event->payload.size == strlen("Gi=1,a=T;FIRSTPAYLOAD"));
    CHECK(memcmp(event->payload.data, "Gi=1,a=T;FIRSTPAYLOAD", event->payload.size) == 0);
    event = kmx_term_graphics_at(term, 1);
    CHECK(event != NULL);
    CHECK(event->payload.size == strlen("Gi=2,a=p;SECOND"));
    CHECK(memcmp(event->payload.data, "Gi=2,a=p;SECOND", event->payload.size) == 0);
    CHECK(kmx_term_graphics_at(term, 2) == NULL);

    /* The graphics bytes are not text: the visible row is the text only. */
    {
        char row[64];
        int col;
        size_t used = 0;
        for (col = 0; col < 40; col++) {
            const kmx_cell *cell = kmx_grid_cell_const(&grid, 0, col);
            if (cell->chars[0] && cell->chars[0] < 128) row[used++] = (char)cell->chars[0];
        }
        row[used] = '\0';
        CHECK(strncmp(row, "beforemiddleafter", 17) == 0);
    }

    kmx_term_graphics_clear(term);
    CHECK(kmx_term_graphics_count(term) == 0);
    kmx_grid_free(&grid);
    kmx_term_free(term);
}

/* A graphics escape split across feeds is still captured as one event: a PTY
 * read boundary lands wherever it lands. */
static void
test_graphics_split_across_feeds(void) {
    kmx_term *term;
    const kmx_graphics_event *event;
    static const char first[] = "text\033_Gi=9,a=T;PART";
    static const char second[] = "ONE_PARTTWO\033\\tail";
    CHECK(kmx_term_create(&term, 10, 40) == KMX_OK);
    CHECK(kmx_term_feed(term, first, sizeof first - 1) == KMX_OK);
    CHECK(kmx_term_feed(term, second, sizeof second - 1) == KMX_OK);
    CHECK(kmx_term_graphics_count(term) == 1);
    event = kmx_term_graphics_at(term, 0);
    CHECK(event != NULL);
    CHECK(event->payload.size == strlen("Gi=9,a=T;PARTONE_PARTTWO"));
    CHECK(memcmp(event->payload.data, "Gi=9,a=T;PARTONE_PARTTWO",
                 event->payload.size) == 0);
    kmx_term_free(term);
}

/* The decoder faces the network.  Every truncation and every single-byte
 * corruption must be rejected, never crash, and never leave a grid claiming
 * dimensions it does not have. */
static void
test_decoder_rejects_malformed_input(void) {
    harness h;
    kmx_buffer message;
    kmx_grid target;
    size_t prefix;
    size_t position;
    int accepted_corruptions = 0;

    harness_init(&h, 8, 20);
    harness_step(&h, "\033[1;32msome content\033[0m here");
    kmx_buffer_init(&message);
    CHECK(kmx_cells_encode(NULL, &h.current, &message) == KMX_OK);
    CHECK(message.size > 16);

    /* Every proper prefix is incomplete and must be refused. */
    for (prefix = 0; prefix < message.size; prefix++) {
        CHECK(kmx_grid_init(&target, 4, 4) == KMX_OK);
        CHECK(kmx_cells_apply(&target, message.data, prefix) != KMX_OK);
        kmx_grid_free(&target);
    }

    /* Single-byte corruption: some values remain legal encodings, but none may
     * crash, and the great majority must be caught. */
    reset_random();
    for (position = 0; position < message.size; position++) {
        unsigned char saved = message.data[position];
        message.data[position] = (unsigned char)(saved ^ 0xff);
        CHECK(kmx_grid_init(&target, 4, 4) == KMX_OK);
        if (kmx_cells_apply(&target, message.data, message.size) == KMX_OK) {
            accepted_corruptions++;
        }
        kmx_grid_free(&target);
        message.data[position] = saved;
    }
    CHECK(accepted_corruptions * 4 < (int)message.size);

    /* Trailing bytes are a disagreement about the format, not a nicety. */
    CHECK(kmx_buffer_append(&message, "\0", 1) == KMX_OK);
    CHECK(kmx_grid_init(&target, 4, 4) == KMX_OK);
    CHECK(kmx_cells_apply(&target, message.data, message.size) != KMX_OK);
    kmx_grid_free(&target);

    kmx_buffer_free(&message);
    harness_free(&h);
}

static void
test_encode_rejects_bad_arguments(void) {
    kmx_grid grid;
    kmx_buffer out;
    CHECK(kmx_grid_init(&grid, 4, 4) == KMX_OK);
    kmx_buffer_init(&out);
    CHECK(kmx_cells_encode(NULL, NULL, &out) == KMX_ERR_INVALID);
    CHECK(kmx_cells_encode(NULL, &grid, NULL) == KMX_ERR_INVALID);
    CHECK(kmx_cells_encode(&grid, &grid, &out) == KMX_ERR_INVALID);
    CHECK(kmx_cells_apply(NULL, "x", 1) == KMX_ERR_INVALID);
    CHECK(strcmp(kmx_result_string(KMX_OK), "success") == 0);
    kmx_buffer_free(&out);
    kmx_grid_free(&grid);
}

/* ---- compression and synchronisation ---------------------------------- */

static void
test_compression_roundtrip(void) {
    kmx_buffer wire;
    kmx_buffer plain;
    static unsigned char repetitive[4096];
    unsigned char noise[512];
    size_t index;

    memset(repetitive, 'A', sizeof repetitive);
    kmx_buffer_init(&wire);
    kmx_buffer_init(&plain);
    CHECK(kmx_compress(repetitive, sizeof repetitive, &wire) == KMX_OK);
    CHECK(wire.size < sizeof repetitive / 4);
    CHECK(kmx_decompress(wire.data, wire.size, &plain) == KMX_OK);
    CHECK(plain.size == sizeof repetitive);
    CHECK(memcmp(plain.data, repetitive, plain.size) == 0);

    /* Incompressible input must cost framing, not growth. */
    reset_random();
    for (index = 0; index < sizeof noise; index++) {
        noise[index] = (unsigned char)next_random();
    }
    kmx_buffer_reset(&wire);
    kmx_buffer_reset(&plain);
    CHECK(kmx_compress(noise, sizeof noise, &wire) == KMX_OK);
    CHECK(wire.size <= sizeof noise + 8);
    CHECK(kmx_decompress(wire.data, wire.size, &plain) == KMX_OK);
    CHECK(plain.size == sizeof noise);
    CHECK(memcmp(plain.data, noise, plain.size) == 0);

    /* Every proper prefix of a framed message is incomplete. */
    for (index = 0; index < wire.size; index++) {
        kmx_buffer scratch;
        kmx_buffer_init(&scratch);
        CHECK(kmx_decompress(wire.data, index, &scratch) != KMX_OK);
        kmx_buffer_free(&scratch);
    }
    kmx_buffer_free(&wire);
    kmx_buffer_free(&plain);
}

typedef struct {
    kmx_sync *sync;
    kmx_receiver *receiver;
    uint64_t clock;
    size_t wire_bytes;
    size_t messages;
} link;

static void
link_init(link *l, int rows, int cols) {
    memset(l, 0, sizeof *l);
    CHECK(kmx_sync_create(&l->sync, rows, cols) == KMX_OK);
    CHECK(kmx_receiver_create(&l->receiver, rows, cols) == KMX_OK);
}

static void
link_free(link *l) {
    kmx_sync_free(l->sync);
    kmx_receiver_free(l->receiver);
}

/* Advance time past the send interval, poll, and optionally deliver. */
static bool
link_pump(link *l, bool deliver) {
    kmx_buffer message;
    kmx_sync_info info;
    bool produced = false;
    l->clock += KMX_SEND_INTERVAL_MAX_MS;
    kmx_buffer_init(&message);
    CHECK(kmx_sync_poll(l->sync, l->clock, &message, &produced, &info) == KMX_OK);
    if (produced) {
        l->messages++;
        l->wire_bytes += message.size;
        if (deliver) {
            uint64_t sequence = 0;
            CHECK(kmx_receiver_apply(
                l->receiver, message.data, message.size, &sequence) == KMX_OK);
            CHECK(sequence == info.sequence);
            CHECK(kmx_sync_ack(l->sync, sequence) == KMX_OK);
        }
    }
    kmx_buffer_free(&message);
    return produced;
}

static void
test_sync_roundtrip(void) {
    link l;
    link_init(&l, 12, 40);
    CHECK(kmx_sync_feed(l.sync, "hello", 5) == KMX_OK);
    CHECK(link_pump(&l, true));
    CHECK(kmx_grid_equal(kmx_receiver_grid(l.receiver), kmx_sync_current(l.sync)));

    CHECK(kmx_sync_feed(l.sync, "\r\n\033[1;33mmore\033[0m", 16) == KMX_OK);
    CHECK(link_pump(&l, true));
    CHECK(kmx_grid_equal(kmx_receiver_grid(l.receiver), kmx_sync_current(l.sync)));
    link_free(&l);
}

/* An idle session must cost nothing at all: no state change, no message. */
static void
test_sync_idle_produces_nothing(void) {
    link l;
    int index;
    link_init(&l, 12, 40);
    CHECK(kmx_sync_feed(l.sync, "settled", 7) == KMX_OK);
    CHECK(link_pump(&l, true));
    l.wire_bytes = 0;
    l.messages = 0;
    for (index = 0; index < 100; index++) {
        CHECK(!link_pump(&l, true));
    }
    CHECK(l.messages == 0);
    CHECK(l.wire_bytes == 0);
    link_free(&l);
}

/* Messages that never arrive are superseded, not retransmitted: the receiver
 * that missed everything is brought current by the next message it does get. */
static void
test_sync_lost_messages_are_superseded(void) {
    link l;
    char line[64];
    int index;
    size_t dropped_bytes;

    link_init(&l, 12, 40);
    CHECK(kmx_sync_feed(l.sync, "start", 5) == KMX_OK);
    CHECK(link_pump(&l, true));

    /* Thirty messages produced and thrown away. */
    for (index = 0; index < 30; index++) {
        snprintf(line, sizeof line, "\r\nlost update %d", index);
        CHECK(kmx_sync_feed(l.sync, line, strlen(line)) == KMX_OK);
        link_pump(&l, false);
    }
    dropped_bytes = l.wire_bytes;
    CHECK(dropped_bytes > 0);
    CHECK(!kmx_grid_equal(kmx_receiver_grid(l.receiver), kmx_sync_current(l.sync)));

    /* One delivered message converges the receiver exactly. */
    l.wire_bytes = 0;
    CHECK(kmx_sync_feed(l.sync, "\r\nFINAL", 7) == KMX_OK);
    CHECK(link_pump(&l, true));
    CHECK(kmx_grid_equal(kmx_receiver_grid(l.receiver), kmx_sync_current(l.sync)));
    link_free(&l);
}

/* Bursty output coalesces into the send interval instead of one message per
 * write, which is what keeps a flooding pane from flooding the link. */
static void
test_sync_coalesces_bursts(void) {
    link l;
    kmx_buffer message;
    kmx_sync_info info;
    bool produced = false;
    int index;

    link_init(&l, 12, 40);
    l.clock = 1000;
    kmx_buffer_init(&message);
    CHECK(kmx_sync_poll(l.sync, l.clock, &message, &produced, &info) == KMX_OK);
    kmx_buffer_reset(&message);

    /* A hundred writes inside one interval produce at most one message. */
    for (index = 0; index < 100; index++) {
        char text[32];
        snprintf(text, sizeof text, "burst %d ", index);
        CHECK(kmx_sync_feed(l.sync, text, strlen(text)) == KMX_OK);
        produced = false;
        CHECK(kmx_sync_poll(l.sync, l.clock + 1, &message, &produced, &info) == KMX_OK);
        CHECK(!produced);
    }
    produced = false;
    CHECK(kmx_sync_poll(
        l.sync, l.clock + KMX_SEND_INTERVAL_MAX_MS, &message, &produced, &info) == KMX_OK);
    CHECK(produced);
    kmx_buffer_free(&message);
    link_free(&l);
}

static void
test_sync_resize(void) {
    link l;
    link_init(&l, 10, 30);
    CHECK(kmx_sync_feed(l.sync, "before", 6) == KMX_OK);
    CHECK(link_pump(&l, true));
    CHECK(kmx_sync_resize(l.sync, 18, 50) == KMX_OK);
    CHECK(kmx_sync_feed(l.sync, " after", 6) == KMX_OK);
    CHECK(link_pump(&l, true));
    CHECK(kmx_receiver_grid(l.receiver)->rows == 18);
    CHECK(kmx_receiver_grid(l.receiver)->cols == 50);
    CHECK(kmx_grid_equal(kmx_receiver_grid(l.receiver), kmx_sync_current(l.sync)));
    link_free(&l);
}

static void
test_receiver_rejects_malformed_messages(void) {
    kmx_receiver *receiver;
    static const unsigned char garbage[] = {0x01, 0xff, 0xff, 0xff, 0xff};
    CHECK(kmx_receiver_create(&receiver, 8, 20) == KMX_OK);
    CHECK(kmx_receiver_apply(receiver, garbage, sizeof garbage, NULL) != KMX_OK);
    CHECK(kmx_receiver_apply(receiver, "", 0, NULL) != KMX_OK);
    CHECK(kmx_receiver_apply(receiver, "\x01", 1, NULL) != KMX_OK);
    kmx_receiver_free(receiver);
}

int
main(void) {
    RUN(test_grid_basics);
    RUN(test_roundtrip_text);
    RUN(test_roundtrip_attributes);
    RUN(test_roundtrip_wide_and_combining);
    RUN(test_roundtrip_scrolling);
    RUN(test_roundtrip_randomised);
    RUN(test_stale_receiver_catches_up_in_one_message);
    RUN(test_small_change_is_a_small_message);
    RUN(test_resize);
    RUN(test_graphics_captured_in_wire_order);
    RUN(test_graphics_split_across_feeds);
    RUN(test_decoder_rejects_malformed_input);
    RUN(test_encode_rejects_bad_arguments);
    RUN(test_compression_roundtrip);
    RUN(test_sync_roundtrip);
    RUN(test_sync_idle_produces_nothing);
    RUN(test_sync_lost_messages_are_superseded);
    RUN(test_sync_coalesces_bursts);
    RUN(test_sync_resize);
    RUN(test_receiver_rejects_malformed_messages);
    puts("all kilix-multiplexer tests passed");
    return 0;
}
