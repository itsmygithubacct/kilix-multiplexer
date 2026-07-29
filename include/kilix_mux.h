#ifndef KILIX_MUX_H
#define KILIX_MUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KMX_VERSION_MAJOR 0
#define KMX_VERSION_MINOR 1
#define KMX_VERSION_PATCH 0

/* Combining characters per cell.  Matches libvterm's own limit so a snapshot
 * can never lose a character the terminal accepted. */
#define KMX_MAX_CHARS 6

/* Grid bounds.  A peer's claimed size is an instruction to allocate, so it is
 * bounded rather than trusted.  The area bound is the one that matters:
 * 2000x2000 satisfies both per-axis limits while asking for four million
 * cells.  500,000 is roughly five times the largest plausible real terminal -
 * a 4K display at a very small font is about 600x160, or 96,000 cells. */
#define KMX_MAX_DIMENSION 2000
#define KMX_MAX_CELLS 500000

typedef enum {
    KMX_OK = 0,
    KMX_ERR_INVALID = 1,
    KMX_ERR_MEMORY = 2,
    KMX_ERR_PROTOCOL = 3,
    KMX_ERR_TRUNCATED = 4,
    KMX_ERR_LIMIT = 5
} kmx_result;

const char *kmx_result_string(kmx_result result);

/* ---- colour ----------------------------------------------------------- */

typedef enum {
    KMX_COLOR_DEFAULT = 0,
    KMX_COLOR_INDEXED = 1,
    KMX_COLOR_RGB = 2
} kmx_color_kind;

typedef struct {
    uint8_t kind;
    uint8_t r;   /* palette index when kind is indexed */
    uint8_t g;
    uint8_t b;
} kmx_color;

/* ---- cells ------------------------------------------------------------ */

#define KMX_ATTR_BOLD 0x0001u
#define KMX_ATTR_UNDERLINE 0x0002u
#define KMX_ATTR_ITALIC 0x0004u
#define KMX_ATTR_BLINK 0x0008u
#define KMX_ATTR_REVERSE 0x0010u
#define KMX_ATTR_CONCEAL 0x0020u
#define KMX_ATTR_STRIKE 0x0040u
#define KMX_ATTR_DWL 0x0080u
#define KMX_ATTR_DHL_TOP 0x0100u
#define KMX_ATTR_DHL_BOTTOM 0x0200u
#define KMX_ATTR_SMALL 0x0400u
#define KMX_ATTR_BASELINE_RAISE 0x0800u
#define KMX_ATTR_BASELINE_LOWER 0x1000u

typedef struct {
    uint32_t chars[KMX_MAX_CHARS]; /* NUL terminated unless full */
    uint16_t attrs;
    uint8_t width;                 /* 1 or 2; 0 marks the tail of a wide cell */
    uint8_t underline;             /* underline style, 0 when not underlined */
    kmx_color fg;
    kmx_color bg;
} kmx_cell;

bool kmx_cell_equal(const kmx_cell *a, const kmx_cell *b);

/* ---- grid ------------------------------------------------------------- */

typedef struct {
    int rows;
    int cols;
    int cursor_row;
    int cursor_col;
    bool cursor_visible;
    kmx_cell *cells; /* rows * cols, row major */
} kmx_grid;

kmx_result kmx_grid_init(kmx_grid *grid, int rows, int cols);
void kmx_grid_free(kmx_grid *grid);
kmx_result kmx_grid_resize(kmx_grid *grid, int rows, int cols);
kmx_result kmx_grid_copy(kmx_grid *destination, const kmx_grid *source);
bool kmx_grid_equal(const kmx_grid *a, const kmx_grid *b);
kmx_cell *kmx_grid_cell(kmx_grid *grid, int row, int col);
const kmx_cell *kmx_grid_cell_const(const kmx_grid *grid, int row, int col);

/* ---- byte buffer ------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} kmx_buffer;

void kmx_buffer_init(kmx_buffer *buffer);
void kmx_buffer_free(kmx_buffer *buffer);
void kmx_buffer_reset(kmx_buffer *buffer);
kmx_result kmx_buffer_append(kmx_buffer *buffer, const void *data, size_t size);

/* ---- terminal model --------------------------------------------------- */

/* A graphics escape the pane emitted, captured whole and in wire order
 * relative to the cell changes around it.  The multiplexer needs these
 * separately from the text plane because they are coded differently and
 * because their payloads may not be re-sendable verbatim. */
typedef struct {
    size_t offset; /* stream position of the first byte fed that produced it */
    kmx_buffer payload;
} kmx_graphics_event;

typedef struct kmx_term kmx_term;

kmx_result kmx_term_create(kmx_term **out, int rows, int cols);
void kmx_term_free(kmx_term *term);
kmx_result kmx_term_feed(kmx_term *term, const void *data, size_t size);
kmx_result kmx_term_resize(kmx_term *term, int rows, int cols);
/* Copy the current screen into `grid`, resizing it if needed. */
kmx_result kmx_term_snapshot(kmx_term *term, kmx_grid *grid);
size_t kmx_term_graphics_count(const kmx_term *term);
const kmx_graphics_event *kmx_term_graphics_at(const kmx_term *term, size_t index);
void kmx_term_graphics_clear(kmx_term *term);

/* ---- cell plane codec ------------------------------------------------- */

/* Encode the difference between `previous` and `current` into `out`.
 *
 * The encoding is a diff against a state the peer is known to hold, never a
 * queue of past updates: a peer that missed everything since `previous` is
 * brought to `current` by this one message, which is what keeps a stalled link
 * from accumulating work. */
kmx_result kmx_cells_encode(
    const kmx_grid *previous,
    const kmx_grid *current,
    kmx_buffer *out
);

/* Apply an encoded diff to `grid`, which must hold the same state the encoder
 * called `previous`. */
kmx_result kmx_cells_apply(kmx_grid *grid, const void *data, size_t size);

/* ---- compression ------------------------------------------------------ */

/* Wrap an encoded message for the wire, compressing when that is smaller.
 * The framing records which it chose, so an incompressible message costs one
 * byte rather than growing. */
kmx_result kmx_compress(const void *data, size_t size, kmx_buffer *out);
kmx_result kmx_decompress(const void *data, size_t size, kmx_buffer *out);

/* ---- state synchronisation -------------------------------------------- */

/* The sender never holds a queue of things to send.  It holds the current
 * state and the last state the receiver acknowledged, and always transmits the
 * difference between them.  Output produced while the link is down therefore
 * costs nothing when it returns: only the final screen is sent.
 *
 * Borrowed in design from mosh's state synchronisation protocol; none of its
 * code is used, and it is GPL where this is MIT. */

/* Bounds on how often a message may be produced.  A busy pane coalesces into
 * the slower bound rather than emitting a message per write. */
#define KMX_SEND_INTERVAL_MIN_MS 20
#define KMX_SEND_INTERVAL_MAX_MS 250

typedef struct kmx_sync kmx_sync;

typedef struct {
    uint64_t sequence;      /* of the message just produced */
    size_t raw_bytes;       /* encoded size before compression */
    size_t wire_bytes;      /* what would go on the wire */
    bool from_scratch;      /* a full repaint rather than a diff */
} kmx_sync_info;

kmx_result kmx_sync_create(kmx_sync **out, int rows, int cols);
void kmx_sync_free(kmx_sync *sync);
kmx_result kmx_sync_feed(kmx_sync *sync, const void *data, size_t size);
kmx_result kmx_sync_resize(kmx_sync *sync, int rows, int cols);

/* Produce a message if the state has moved on and the send interval permits.
 * `produced` is false when there is nothing to say, which is what makes an
 * idle session cost nothing. */
kmx_result kmx_sync_poll(
    kmx_sync *sync,
    uint64_t now_millis,
    kmx_buffer *out,
    bool *produced,
    kmx_sync_info *info
);

/* Acknowledge everything up to and including `sequence`.  Until an ack
 * arrives the sender keeps diffing against the last acknowledged state, so a
 * lost message is superseded by the next one rather than retransmitted. */
kmx_result kmx_sync_ack(kmx_sync *sync, uint64_t sequence);

/* Set the interval between messages, clamped to the bounds above. */
void kmx_sync_set_interval(kmx_sync *sync, unsigned millis);

const kmx_grid *kmx_sync_current(const kmx_sync *sync);
kmx_term *kmx_sync_term(kmx_sync *sync);

/* Receiver half: apply a wire message and expose the resulting screen. */
typedef struct kmx_receiver kmx_receiver;

kmx_result kmx_receiver_create(kmx_receiver **out, int rows, int cols);
void kmx_receiver_free(kmx_receiver *receiver);
kmx_result kmx_receiver_apply(
    kmx_receiver *receiver,
    const void *data,
    size_t size,
    uint64_t *sequence
);
const kmx_grid *kmx_receiver_grid(const kmx_receiver *receiver);

#ifdef __cplusplus
}
#endif

#endif
