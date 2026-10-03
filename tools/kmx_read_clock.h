#ifndef KMX_READ_CLOCK_H
#define KMX_READ_CLOCK_H

#include <stddef.h>
#include <stdint.h>

/* Reads are paused behind a complete backpressured frame. Otherwise the
 * retained prefix is at most one incomplete frame, so one prefix boundary
 * tracks its oldest contributing read without allocating a timestamp queue. */
typedef struct {
    size_t old_bytes;
    uint64_t earliest_ms, latest_ms;
} kmx_read_clock;

static inline void kmx_read_clock_push(kmx_read_clock *clock, size_t retained, uint64_t received_ms) {
    clock->old_bytes = retained;
    clock->latest_ms = received_ms;
    if (!retained) clock->earliest_ms = received_ms;
}

static inline void kmx_read_clock_consume(kmx_read_clock *clock, size_t consumed) {
    if (consumed >= clock->old_bytes) {
        clock->old_bytes = 0;
        clock->earliest_ms = clock->latest_ms;
    } else clock->old_bytes -= consumed;
}

#endif
