/* Ordered-stream regressions for overlapping acknowledged diffs. */
#include "kilix_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define OK(x) CHECK((x) == KMX_OK)

typedef struct {
    kmx_sync *sync;
    kmx_receiver *receiver;
    uint64_t now;
    uint64_t latest;
    size_t framed_bytes;
    unsigned messages;
} harness;

static void init(harness *h, int rows, int cols) {
    memset(h, 0, sizeof *h);
    OK(kmx_sync_create(&h->sync, rows, cols));
    OK(kmx_receiver_create(&h->receiver, rows, cols));
    kmx_sync_set_interval(h->sync, KMX_SEND_INTERVAL_MIN_MS);
    h->now = 1000;
}
static void done(harness *h) {
    kmx_sync_free(h->sync);
    kmx_receiver_free(h->receiver);
}
static void feed(harness *h, const char *text) {
    OK(kmx_sync_feed(h->sync, text, strlen(text)));
}
static bool poll_state(harness *h, bool deliver, bool exact, bool *full) {
    kmx_buffer out, payload, framed;
    kmx_sync_info info;
    bool produced = false;
    kmx_buffer_init(&out);
    h->now += 100;
    OK(kmx_sync_poll(h->sync, h->now, &out, &produced, &info));
    if (produced) {
        uint64_t received = 0;
        h->latest = info.sequence;
        if (full) *full = info.from_scratch;
        /* Match kmx-serve framing: pane byte + synchronizer + KMX frame. */
        kmx_buffer_init(&payload);
        kmx_buffer_init(&framed);
        OK(kmx_buffer_append(&payload, &(unsigned char){0}, 1));
        OK(kmx_buffer_append(&payload, out.data, out.size));
        OK(kmx_frame_encode(KMX_MSG_CELLS, payload.data, payload.size, &framed));
        h->framed_bytes += framed.size;
        h->messages++;
        kmx_buffer_free(&framed);
        kmx_buffer_free(&payload);
        if (deliver) {
            OK(kmx_receiver_apply(h->receiver, out.data, out.size, &received));
            CHECK(received == info.sequence);
            if (exact) CHECK(kmx_grid_equal(kmx_sync_current(h->sync),
                                          kmx_receiver_grid(h->receiver)));
        }
    }
    kmx_buffer_free(&out);
    return produced;
}
static uint64_t send_state(harness *h) {
    CHECK(poll_state(h, true, true, NULL));
    return h->latest;
}
static void ack(harness *h, uint64_t seq) {
    OK(kmx_sync_ack_at(h->sync, seq, h->now + 10));
}
static void idle(harness *h) {
    ack(h, h->latest);
    for (int i = 0; i < 3; i++) CHECK(!poll_state(h, true, true, NULL));
}
static void baseline(harness *h) {
    feed(h, "\033[HA\033[2;1Hx");
    ack(h, send_state(h));
}

static void reversion(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); send_state(&h);
    feed(&h, "\033[HA\033[2;1Hy"); send_state(&h);
    idle(&h); done(&h);
}
static void return_to_entire_baseline(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); send_state(&h);
    /* Restore both text and the baseline cursor. An equal acknowledged grid
     * must not suppress a repair while B can still be at the receiver. */
    feed(&h, "\033[HA\033[2;2H"); send_state(&h);
    idle(&h); done(&h);
}
static void cursor_only_reversion(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[4;8H"); send_state(&h);
    feed(&h, "\033[2;2H"); send_state(&h);
    idle(&h); done(&h);
}
static void ack_while_newer_messages_are_in_flight(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); uint64_t b = send_state(&h);
    feed(&h, "\033[HC\033[3;1Hz"); uint64_t c = send_state(&h);
    feed(&h, "\033[HA\033[3;1H \033[2;1Hy"); send_state(&h);
    ack(&h, b);
    feed(&h, "\033[HB\033[2;1Hx"); send_state(&h);
    ack(&h, c); /* A cumulative ACK of an older applied screen. */
    feed(&h, "\033[HC\033[4;1Hnew"); send_state(&h);
    OK(kmx_sync_ack(h.sync, b)); /* Stale/duplicate ACK cannot undo progress. */
    idle(&h); done(&h);
}
static void skipped_messages(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); CHECK(poll_state(&h, false, false, NULL));
    feed(&h, "\033[HC\033[3;1Hz"); send_state(&h);
    feed(&h, "\033[HA\033[3;1H "); CHECK(poll_state(&h, false, false, NULL));
    feed(&h, "\033[4;1Hnew"); send_state(&h);
    idle(&h); done(&h);
}
static void evicted_history(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); send_state(&h);
    feed(&h, "\033[2;1Hy"); uint64_t evicted = send_state(&h);
    /* More than the eight saved states; a repair obligation must outlive its
     * originating state, including when that row never changes again. */
    feed(&h, "\033[HA"); CHECK(poll_state(&h, false, false, NULL));
    for (int i = 0; i < 24; i++) {
        char text[64]; snprintf(text, sizeof text, "\033[2;1H%03d", i);
        feed(&h, text); CHECK(poll_state(&h, false, false, NULL));
    }
    ack(&h, evicted); /* An evicted ACK must not erase repair obligations. */
    feed(&h, "\033[3;1Hlast"); send_state(&h);
    idle(&h); done(&h);
}
static void style_and_wide_character_reversion(void) {
    harness h; init(&h, 6, 20);
    feed(&h, "\033[31;1m界é\033[0m\033[2;1Hx");
    ack(&h, send_state(&h));
    feed(&h, "\033[H\033[32;3mXXe \033[0m"); send_state(&h);
    feed(&h, "\033[H\033[31;1m界é\033[0m\033[2;1Hy"); send_state(&h);
    idle(&h); done(&h);
}
static void scrolling(void) {
    harness h; init(&h, 5, 20);
    feed(&h, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    ack(&h, send_state(&h));
    for (int i = 0; i < 16; i++) {
        char text[40]; snprintf(text, sizeof text, "\r\nline %d", i);
        feed(&h, text); send_state(&h);
    }
    feed(&h, "\033[2J\033[Hone\r\ntwo\r\nthree\r\nfour\r\nfive");
    send_state(&h); idle(&h); done(&h);
}
static void resize_and_restore_dimensions(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    OK(kmx_sync_resize(h.sync, 3, 12)); feed(&h, "\033[HB"); send_state(&h);
    OK(kmx_sync_resize(h.sync, 6, 20));
    feed(&h, "\033[2J\033[HA\033[2;1Hy"); send_state(&h);
    OK(kmx_sync_resize(h.sync, 4, 25)); send_state(&h);
    feed(&h, "\033[3;1Hwide 界"); send_state(&h);
    idle(&h); done(&h);
}
static void reset_with_late_ack(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); uint64_t old = send_state(&h);
    kmx_sync_reset_baseline(h.sync);
    kmx_receiver_free(h.receiver);
    OK(kmx_receiver_create(&h.receiver, 2, 2));
    ack(&h, old);
    feed(&h, "\033[HA\033[2;1Hy");
    bool full = false;
    CHECK(poll_state(&h, true, true, &full)); CHECK(full);
    feed(&h, "\033[3;1Hafter reset"); send_state(&h);
    ack(&h, old);
    idle(&h); done(&h);
}
static void repeated_unchanged_updates_settle(void) {
    harness h; init(&h, 6, 20); baseline(&h);
    feed(&h, "\033[HB"); uint64_t changed = send_state(&h);
    for (int i = 0; i < 16; i++) send_state(&h);
    /* ACK a retained repeat, not the latest message. Newer messages contain
     * the same rows, so acknowledging this state should make us idle. */
    ack(&h, h.latest - 3);
    CHECK(!poll_state(&h, true, true, NULL));
    OK(kmx_sync_ack(h.sync, changed));
    CHECK(!poll_state(&h, true, true, NULL));
    done(&h);
}

static uint32_t random_state = 0x13298745u;
static uint32_t rng(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static void randomized_schedules(void) {
    for (int schedule = 0; schedule < 1000; schedule++) {
        harness h; init(&h, 6, 20); baseline(&h);
        uint64_t last_delivered = h.latest;
        uint64_t delayed_ack = 0;
        for (int step = 0; step < 32; step++) {
            unsigned pick = rng();
            char text[128];
            if ((pick & 31u) == 0) {
                OK(kmx_sync_resize(h.sync, 3 + (int)(rng() % 5), 12 + (int)(rng() % 12)));
            } else if ((pick & 15u) == 0) {
                snprintf(text, sizeof text, "\r\nscroll %d", step);
                feed(&h, text);
            } else {
                snprintf(text, sizeof text, "\033[%u;%uH\033[%um%c",
                         rng() % 3 + 1, rng() % 10 + 1,
                         rng() % 2 ? 0 : 31, (char)('A' + rng() % 3));
                feed(&h, text);
            }
            bool deliver = (rng() % 4) != 0;
            if (poll_state(&h, deliver, true, NULL) && deliver) {
                delayed_ack = last_delivered;
                last_delivered = h.latest;
            }
            if ((rng() % 4) == 0) ack(&h, last_delivered);
            if (delayed_ack && (rng() % 8) == 0) ack(&h, delayed_ack);
        }
        /* Settle with either a repair or an already matching idle screen. */
        if (poll_state(&h, true, true, NULL)) last_delivered = h.latest;
        ack(&h, last_delivered);
        CHECK(kmx_grid_equal(kmx_sync_current(h.sync), kmx_receiver_grid(h.receiver)));
        CHECK(!poll_state(&h, true, true, NULL));
        done(&h);
    }
}

static void measure(const char *name, bool repaint) {
    harness h; init(&h, 24, 80);
    char text[256];
    for (int row = 0; row < 24; row++) {
        snprintf(text, sizeof text, "\033[%d;1Hrow %02d: abcdefghijklmnopqrstuvwxyz 0123456789", row + 1, row);
        feed(&h, text);
    }
    CHECK(poll_state(&h, true, false, NULL)); ack(&h, h.latest);
    h.framed_bytes = 0; h.messages = 0;
    unsigned divergent = 0;
    for (int i = 0; i < 240; i++) {
        if (repaint) {
            /* Each repaint clears and reconstructs the display but mostly
             * leaves stable text. A status row changes on every frame. */
            feed(&h, "\033[H\033[2J");
            for (int row = 0; row < 24; row++) {
                snprintf(text, sizeof text, "\033[%d;1Hrow %02d: abcdefghijklmnopqrstuvwxyz 0123456789", row + 1, row);
                feed(&h, text);
            }
            snprintf(text, sizeof text, "\033[24;60Htick %03d", i); feed(&h, text);
        } else {
            snprintf(text, sizeof text, "\033[1;1Hrow %02d\033[2;60Htick %03d", i % 3 == 1 ? 99 : 0, i);
            feed(&h, text);
        }
        CHECK(poll_state(&h, true, false, NULL));
        if (!kmx_grid_equal(kmx_sync_current(h.sync), kmx_receiver_grid(h.receiver))) divergent++;
        if (i % 3 == 2) ack(&h, h.latest); /* 300 ms local delayed-ACK batches. */
    }
    printf("%s frames=%u framed_cell_bytes=%zu divergent_frames=%u\n",
           name, h.messages, h.framed_bytes, divergent);
    done(&h);
}

/* A deterministic link: frame serialization at 32 KiB/s, one-way delay,
 * immediate decoding/ACK generation and the same return-path delay. Nothing
 * sleeps or opens sockets. Pending messages are bounded test-owned events. */
#define SLOW_LINK_EVENTS 4096
typedef struct {
    kmx_buffer message;
    kmx_grid expected;
    uint64_t sequence;
    uint64_t received_at;
    uint64_t ack_at;
    size_t framed_bytes;
    bool delivered;
} slow_event;
typedef struct {
    size_t sent_bytes;
    size_t received_bytes;
    size_t idle_sent_bytes;
    size_t idle_received_bytes;
    unsigned sent_messages;
    unsigned skipped_messages;
    unsigned rtt;
    unsigned interval;
    bool equal;
} slow_result;

static void coding_screen(harness *h) {
    for (int row = 0; row < 24; row++) {
        char text[160];
        snprintf(text, sizeof text,
                 "\033[%d;1Hsrc/module_%02d.c:%03d  if (ready) { return value_%02d; } // coding",
                 row + 1, row, 100 + row * 7, row);
        feed(h, text);
    }
    feed(h, "\033[2;60HSTATUS");
}
static slow_result slow_link(unsigned rtt, bool changes, unsigned skip_first,
                             bool pinned, bool verify) {
    harness h; init(&h, 24, 80);
    if (!pinned) {
        /* init() normally pins pacing for the reversion tests. This link
         * must start with the normal, adaptive synchronizer instead. */
        kmx_sync_free(h.sync); OK(kmx_sync_create(&h.sync, 24, 80));
    }
    coding_screen(&h);
    slow_event *events = calloc(SLOW_LINK_EVENTS, sizeof *events);
    CHECK(events);
    unsigned queued = 0, receive_index = 0, ack_index = 0;
    unsigned transitions = 0;
    uint64_t serialization_end = 0;
    uint64_t change_until = changes ? rtt * 2u : 0;
    /* Deliberately discarding N unchanged startup retries now takes up to N
     * seconds before the first deliverable screen. Allow that recovery phase
     * before asserting a fully settled one-second idle window. */
    uint64_t idle_start = change_until + skip_first * (pinned ? 20u : 1000u) +
        rtt * 3u + 2000u;
    uint64_t finish = idle_start + 1000u;
    slow_result result = {0};
    /* 10 ms granularity; each RTT case is an exact multiple of that. */
    for (uint64_t elapsed = 0; elapsed < finish; elapsed += 10) {
        uint64_t now = 1000 + elapsed;
        while (receive_index < queued && events[receive_index].received_at <= now) {
            slow_event *e = &events[receive_index++];
            uint64_t sequence = 0;
            OK(kmx_receiver_apply(h.receiver, e->message.data, e->message.size, &sequence));
            CHECK(sequence == e->sequence);
            CHECK(kmx_grid_equal(&e->expected, kmx_receiver_grid(h.receiver)));
            result.received_bytes += e->framed_bytes;
            if (elapsed >= idle_start) result.idle_received_bytes += e->framed_bytes;
            kmx_buffer_free(&e->message); kmx_grid_free(&e->expected);
            e->delivered = true;
        }
        while (ack_index < queued && events[ack_index].ack_at <= now) {
            slow_event *e = &events[ack_index++];
            CHECK(e->delivered);
            OK(kmx_sync_ack_at(h.sync, e->sequence, now));
        }
        if (changes && elapsed > 0 && elapsed <= change_until &&
            elapsed % 70u == 0) {
            char text[128];
            /* Restore a row independently of the status row. Updates occur
             * throughout the first two RTTs, while old ACKs are returning. */
            snprintf(text, sizeof text, "\033[1;1H%s\033[2;60H%06u",
                     transitions % 3 == 1 ? "MODIFIED     " : "src/module_00",
                     transitions);
            transitions++;
            feed(&h, text);
        }
        kmx_buffer message, payload, framed;
        kmx_buffer_init(&message); kmx_buffer_init(&payload); kmx_buffer_init(&framed);
        kmx_sync_info info; bool produced = false;
        OK(kmx_sync_poll(h.sync, now, &message, &produced, &info));
        if (produced) {
            OK(kmx_buffer_append(&payload, &(unsigned char){0}, 1));
            OK(kmx_buffer_append(&payload, message.data, message.size));
            OK(kmx_frame_encode(KMX_MSG_CELLS, payload.data, payload.size, &framed));
            result.sent_bytes += framed.size;
            if (elapsed >= idle_start) result.idle_sent_bytes += framed.size;
            result.sent_messages++;
            if (result.sent_messages <= skip_first) {
                /* A skipped message gets no ACK. The next delivered message
                 * must describe the latest screen, including all reversions. */
                result.skipped_messages++;
            } else {
                CHECK(queued < SLOW_LINK_EVENTS);
                slow_event *e = &events[queued++];
                e->message = message; kmx_buffer_init(&message);
                e->sequence = info.sequence; e->framed_bytes = framed.size;
                OK(kmx_grid_copy(&e->expected, kmx_sync_current(h.sync)));
                if (serialization_end < now) serialization_end = now;
                serialization_end += (framed.size * 1000u + 32767u) / 32768u;
                e->received_at = serialization_end + rtt / 2u;
                e->ack_at = e->received_at + rtt / 2u;
            }
        }
        kmx_buffer_free(&message); kmx_buffer_free(&payload); kmx_buffer_free(&framed);
    }
    result.rtt = kmx_sync_rtt_millis(h.sync);
    result.interval = kmx_sync_interval_millis(h.sync);
    result.equal = kmx_grid_equal(kmx_sync_current(h.sync), kmx_receiver_grid(h.receiver));
    if (verify) {
        CHECK(result.equal);
        CHECK(result.idle_sent_bytes == 0);
        CHECK(result.idle_received_bytes == 0);
        CHECK(result.rtt >= rtt);
        CHECK(pinned ? result.interval == KMX_SEND_INTERVAL_MIN_MS
                     : result.interval > KMX_SEND_INTERVAL_MIN_MS);
        CHECK(ack_index == queued); /* The test did not hide a backlog. */
    }
    for (unsigned i = receive_index; i < queued; i++) {
        kmx_buffer_free(&events[i].message); kmx_grid_free(&events[i].expected);
    }
    free(events); done(&h);
    return result;
}
static void initial_retry_remains_bounded_and_recovers(void) {
    harness h;
    init(&h, 6, 20);
    kmx_sync_free(h.sync);
    h.sync = NULL;
    OK(kmx_sync_create(&h.sync, 6, 20));
    feed(&h, "A");
    CHECK(poll_state(&h, false, false, NULL)); /* 1100: first snapshot skipped */
    CHECK(!poll_state(&h, true, true, NULL)); /* 1200: no initial flood */
    feed(&h, "B");
    CHECK(poll_state(&h, false, false, NULL)); /* 1300: fresh content still sends */
    h.now = 2100;
    CHECK(!poll_state(&h, true, true, NULL)); /* 2200: allow ACK time */
    CHECK(poll_state(&h, true, true, NULL)); /* 2300: recover both skipped screens */
    idle(&h);
    done(&h);
}

static void unchanged_retries_do_not_delay_new_content(void) {
    harness h;
    init(&h, 6, 20);
    /* Let real RTT feedback select the cadence instead of init's pin. */
    kmx_sync_free(h.sync);
    h.sync = NULL;
    OK(kmx_sync_create(&h.sync, 6, 20));
    feed(&h, "A");
    uint64_t initial = send_state(&h); /* 1100 ms */
    OK(kmx_sync_ack_at(h.sync, initial, 1600));
    CHECK(kmx_sync_rtt_millis(h.sync) == 500);
    h.now = 1600;
    feed(&h, "B");
    send_state(&h); /* 1700 ms, deliberately unacknowledged */
    h.now = 1850;
    CHECK(!poll_state(&h, true, true, NULL)); /* 1950: no redundant retry */
    feed(&h, "C");
    CHECK(poll_state(&h, false, false, NULL)); /* 2050: new content proceeds */
    h.now = 2850;
    CHECK(!poll_state(&h, true, true, NULL)); /* 2950: still awaiting ACK */
    CHECK(poll_state(&h, true, true, NULL)); /* 3050: skipped C is repaired */
    idle(&h);
    done(&h);
}

static void slow_link_regressions(void) {
    initial_retry_remains_bounded_and_recovers();
    unchanged_retries_do_not_delay_new_content();
    const unsigned rtts[] = {500, 1200, 3000, 10000};
    for (size_t i = 0; i < sizeof rtts / sizeof rtts[0]; i++) {
        (void)slow_link(rtts[i], false, 0, false, true);
        (void)slow_link(rtts[i], false, 12, false, true);
        (void)slow_link(rtts[i], true, 0, false, true);
        (void)slow_link(rtts[i], true, 12, false, true);
    }
    /* Explicitly pinned pacing must also settle without evicting all useful
     * ACK evidence; changing the caller's chosen interval is not the fix. */
    (void)slow_link(3000, true, 12, true, true);
}
static void measure_slow_links(void) {
    const unsigned rtts[] = {500, 1200, 3000, 10000};
    for (size_t i = 0; i < sizeof rtts / sizeof rtts[0]; i++) {
        for (unsigned variant = 0; variant < 4; variant++) {
            bool changes = variant >= 2;
            unsigned skip = variant % 2 ? 12 : 0;
            slow_result r = slow_link(rtts[i], changes, skip, false, false);
            printf("rtt=%u changes=%d skip=%u messages=%u framed_bytes=%zu received_bytes=%zu idle_sent_bytes=%zu idle_received_bytes=%zu learned_rtt=%u interval=%u equal=%d\n",
                   rtts[i], changes, skip, r.sent_messages, r.sent_bytes, r.received_bytes,
                   r.idle_sent_bytes, r.idle_received_bytes, r.rtt, r.interval, r.equal);
        }
    }
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--measure-pacing") == 0) {
        measure_slow_links();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--test-pacing") == 0) {
        slow_link_regressions();
        puts("slow-link pacing regressions passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--measure") == 0) {
        measure("frequent-repaint", true);
        measure("changed-then-restored", false);
        return 0;
    }
    reversion();
    return_to_entire_baseline();
    cursor_only_reversion();
    ack_while_newer_messages_are_in_flight();
    skipped_messages();
    evicted_history();
    style_and_wide_character_reversion();
    scrolling();
    resize_and_restore_dimensions();
    reset_with_late_ack();
    repeated_unchanged_updates_settle();
    randomized_schedules();
    slow_link_regressions();
    puts("sync in-flight regressions, 1000 randomized schedules and 17 slow-link cases passed");
    return 0;
}
