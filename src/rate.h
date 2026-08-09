#ifndef KILIX_MUX_RATE_H
#define KILIX_MUX_RATE_H

/* The sliding-second allowance shared by the planes that are allowed to drop.
 *
 * Internal to the library; the planes expose only `bytes_per_second`.
 *
 * A window that simply zeroed its counter every second did not bound anything:
 * a frame or block larger than the whole allowance was admitted in full, the
 * counter was reset the next second, and the same thing happened again. The
 * documented ceiling was therefore exceeded indefinitely rather than once, and
 * with the drop counter reading zero the whole time, because the gate never
 * saw a window it considered spent.
 *
 * Carrying the overspend forward is what makes it a ceiling. Each elapsed
 * second repays one budget's worth; anything still owed keeps the gate shut,
 * so a plane that overshoots goes quiet until it has paid for what it sent.
 * The long-run rate is then bounded by the budget, while a single frame is
 * still delivered whole rather than truncated into something undecodable. */

#include <stdint.h>
#include <stddef.h>

static inline void
rate_window_advance(
    uint64_t *window_start,
    size_t *window_bytes,
    uint32_t budget,
    uint64_t now_millis
) {
    uint64_t elapsed;
    uint64_t windows;
    uint64_t repaid;

    if (!budget || now_millis < *window_start + 1000u) return;

    elapsed = now_millis - *window_start;
    windows = elapsed / 1000u;

    /* budget >= 1, so `windows` seconds repay at least `windows` bytes. When
     * that already covers the debt the account is simply clear, and computing
     * the product would be both pointless and the only place this arithmetic
     * could overflow. */
    if (windows >= (uint64_t)*window_bytes) {
        *window_bytes = 0;
        *window_start = now_millis;
        return;
    }
    repaid = windows * (uint64_t)budget;
    *window_bytes = repaid >= (uint64_t)*window_bytes
        ? 0u
        : (size_t)((uint64_t)*window_bytes - repaid);
    /* Advance in whole seconds so the window keeps its phase rather than
     * restarting on every offer. */
    *window_start += windows * 1000u;
}

#endif /* KILIX_MUX_RATE_H */
