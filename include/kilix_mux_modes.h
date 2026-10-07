#ifndef KILIX_MUX_MODES_H
#define KILIX_MUX_MODES_H

#include "kilix_mux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KMX_MODE_APPLICATION_CURSOR UINT32_C(0x01)
#define KMX_MODE_BRACKETED_PASTE    UINT32_C(0x02)
#define KMX_MODE_FOCUS_REPORT       UINT32_C(0x04)
#define KMX_MODE_MOUSE_CLICK        UINT32_C(0x08)
#define KMX_MODE_MOUSE_DRAG         UINT32_C(0x10)
#define KMX_MODE_MOUSE_MOVE         UINT32_C(0x20)
#define KMX_MODE_MOUSE_SGR          UINT32_C(0x40)
#define KMX_MODE_MOUSE_TRACKING \
    (KMX_MODE_MOUSE_CLICK | KMX_MODE_MOUSE_DRAG | KMX_MODE_MOUSE_MOVE)
#define KMX_MODE_ALL UINT32_C(0x7f)

typedef struct kmx_modes kmx_modes;

/* A parser-only observer: no screen, terminal replies, or retained output.
 * Feed exactly the PTY output supplied to the terminal model, in order.
 * Enhanced keyboard protocols and application keypad mode are unsupported. */
kmx_result kmx_modes_create(kmx_modes **out);
void kmx_modes_free(kmx_modes *modes);
kmx_result kmx_modes_feed(kmx_modes *modes, const void *data, size_t size);
/* Clear all modes and discard any incomplete escape or string. */
void kmx_modes_reset(kmx_modes *modes);
uint32_t kmx_modes_get(const kmx_modes *modes);
/* DEC synchronized output (?2026) is local display scheduling state, never
 * an input flag or part of the eight-byte terminal-mode wire payload. */
bool kmx_modes_synchronized(const kmx_modes *modes);
/* Changes on an inactive-to-active transition, including an end/new begin
 * within one feed. Repeated sets while active do not change this value. */
uint64_t kmx_modes_synchronized_generation(const kmx_modes *modes);

/* Optional terminal-mode message payload, independent of negotiation/framing:
 * version (1), focused pane (uint8), two reserved zero bytes, flags (BE32).
 * Exactly one mouse tracking bit may be present. Pane identity is validated
 * against the current layout by the caller. Decode leaves outputs untouched
 * on failure; encode leaves the destination untouched on invalid flags. */
#define KMX_MODES_WIRE_SIZE 8u
#define KMX_MODES_WIRE_VERSION 1u
kmx_result kmx_modes_encode(
    uint8_t pane, uint32_t flags, unsigned char out[KMX_MODES_WIRE_SIZE]);
kmx_result kmx_modes_decode(
    const void *wire, size_t size, uint8_t *pane, uint32_t *flags);

#ifdef __cplusplus
}
#endif

#endif
