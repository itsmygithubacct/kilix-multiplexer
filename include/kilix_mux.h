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

/* Synchronise over a terminal someone else owns.
 *
 * What the receiver holds is per receiver; what the pane shows is not.  Two
 * clients watching one pane therefore share its terminal model and keep
 * separate baselines, so each is sent the difference between the screen and
 * what *it* has acknowledged - and neither pays for the other falling behind.
 * The caller must keep `term` alive for the life of the sync and free it
 * itself. */
kmx_result kmx_sync_create_over(kmx_sync **out, kmx_term *term);
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

/* Forget what the receiver is believed to hold, so the next message is a full
 * screen.  This is what a newly attached client needs: it holds nothing, but
 * the pane's history is in the terminal model and must not be discarded with
 * it. */
void kmx_sync_reset_baseline(kmx_sync *sync);

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

/* ---- message framing --------------------------------------------------- */

/* One connection carries several kinds of message.  They are framed with an
 * explicit length so a reader never has to guess where one ends, and typed so
 * the planes can be told apart without inspecting their contents. */
typedef enum {
    KMX_MSG_HELLO = 1,   /* client -> server: dimensions and capabilities */
    KMX_MSG_CELLS = 2,   /* server -> client: a cell-plane message         */
    KMX_MSG_INPUT = 3,   /* client -> server: keystrokes                   */
    KMX_MSG_RESIZE = 4,  /* client -> server: new dimensions               */
    KMX_MSG_ACK = 5,     /* client -> server: sequence received            */
    KMX_MSG_EXIT = 6,    /* server -> client: the session ended            */
    KMX_MSG_LAYOUT = 7,  /* server -> client: the layout plane             */
    KMX_MSG_FOCUS = 8,   /* client -> server: focus a pane                 */
    KMX_MSG_IMAGE = 9    /* server -> client: the still-graphics plane     */
} kmx_message_type;

/* The largest single message accepted from a peer.  A length prefix is an
 * instruction to allocate, so it is bounded rather than trusted. */
#define KMX_MESSAGE_MAX (8u * 1024u * 1024u)

kmx_result kmx_frame_encode(
    kmx_message_type type,
    const void *payload,
    size_t size,
    kmx_buffer *out
);

/* Incremental reader: append bytes as they arrive, take whole messages out.
 * Returns KMX_OK with `ready` false when more bytes are needed. */
typedef struct {
    kmx_buffer pending;
} kmx_framer;

void kmx_framer_init(kmx_framer *framer);
void kmx_framer_free(kmx_framer *framer);
kmx_result kmx_framer_push(kmx_framer *framer, const void *data, size_t size);
kmx_result kmx_framer_next(
    kmx_framer *framer,
    bool *ready,
    kmx_message_type *type,
    const unsigned char **payload,
    size_t *size
);
/* Discard the message most recently returned by kmx_framer_next. */
void kmx_framer_consume(kmx_framer *framer);

/* ---- layout plane ------------------------------------------------------ */

/* Where the panes are.  This is the plane that costs almost nothing and saves
 * the most: the client draws its own chrome from these few numbers instead of
 * being sent pictures of borders and titles. */

#define KMX_MAX_PANES 32
#define KMX_TITLE_MAX 64

typedef struct {
    uint32_t id;
    int32_t x;       /* column of the pane's left edge in the session screen */
    int32_t y;       /* row of the pane's top edge                           */
    int32_t rows;
    int32_t cols;
    bool focused;
    char title[KMX_TITLE_MAX];
} kmx_pane_info;

typedef struct {
    int32_t rows;    /* the whole session screen */
    int32_t cols;
    uint32_t generation; /* bumped whenever the arrangement changes */
    size_t pane_count;
    kmx_pane_info panes[KMX_MAX_PANES];
} kmx_layout;

void kmx_layout_init(kmx_layout *layout, int rows, int cols);
kmx_result kmx_layout_encode(const kmx_layout *layout, kmx_buffer *out);
kmx_result kmx_layout_apply(kmx_layout *layout, const void *data, size_t size);
bool kmx_layout_equal(const kmx_layout *a, const kmx_layout *b);
const kmx_pane_info *kmx_layout_find(const kmx_layout *layout, uint32_t id);

/* Arrange `count` panes in a simple split, filling the session screen.  The
 * server owns geometry so every client agrees about it without negotiating. */
kmx_result kmx_layout_arrange(kmx_layout *layout, size_t count, bool vertical);

/* Draw the panes into one screen, with a one-cell divider between them and a
 * title row per pane when there is more than one.  This is the chrome the
 * client draws for itself rather than receiving as pixels. */
kmx_result kmx_layout_composite(
    const kmx_layout *layout,
    const kmx_grid *const *panes,
    size_t pane_count,
    kmx_grid *out
);

/* ---- still-graphics plane ---------------------------------------------- */

/* Images are addressed by their content, not by when they were sent.  A
 * redraw, a reattach, or the same image appearing twice all become a
 * reference to something the client already holds - which is the difference
 * between a photograph costing once and costing every time the screen
 * repaints.
 *
 * The key is a non-cryptographic 128-bit hash.  It is defending against
 * coincidence, not against an adversary choosing a collision: both ends of
 * this connection are the same user. */
#define KMX_IMAGE_KEY_BYTES 16

typedef struct {
    unsigned char bytes[KMX_IMAGE_KEY_BYTES];
} kmx_image_key;

kmx_image_key kmx_image_key_of(const void *data, size_t size);
bool kmx_image_key_equal(const kmx_image_key *a, const kmx_image_key *b);

typedef struct kmx_image_cache kmx_image_cache;

/* Bounded in both entries and bytes: an image cache that grows without limit
 * is a way for a pane to exhaust memory at both ends. */
kmx_result kmx_image_cache_create(
    kmx_image_cache **out,
    size_t max_entries,
    size_t max_bytes
);
void kmx_image_cache_free(kmx_image_cache *cache);
bool kmx_image_cache_has(const kmx_image_cache *cache, const kmx_image_key *key);
kmx_result kmx_image_cache_put(
    kmx_image_cache *cache,
    const kmx_image_key *key,
    const void *data,
    size_t size
);
const unsigned char *kmx_image_cache_get(
    const kmx_image_cache *cache,
    const kmx_image_key *key,
    size_t *size
);
size_t kmx_image_cache_count(const kmx_image_cache *cache);
size_t kmx_image_cache_bytes(const kmx_image_cache *cache);

/* Wire form of one image event.  `data` may be NULL, which encodes a
 * reference to an image the client is known to hold already. */
kmx_result kmx_image_encode(
    uint32_t pane,
    const kmx_image_key *key,
    const void *data,
    size_t size,
    kmx_buffer *out
);

typedef struct {
    uint32_t pane;
    kmx_image_key key;
    bool has_data;
    const unsigned char *data;
    size_t size;
} kmx_image_message;

kmx_result kmx_image_decode(
    const void *wire,
    size_t wire_size,
    kmx_image_message *message
);

/* ---- motion plane ------------------------------------------------------ */

/* A pane whose content is pixels rather than cells: a browser, a GUI app, a
 * desktop.  Kilix knows which panes these are because it launched them, so
 * unlike a pixel protocol this plane never has to *detect* video - it is told.
 *
 * This is the one plane allowed to drop.  Frames are offered; what is sent is
 * whatever the rate allowance permits, and a frame that does not fit is
 * discarded rather than queued, because the next one supersedes it anyway.
 *
 * The codec here is lossless rectangles.  That is the right default for
 * screen content and it keeps the plane free of an encoder dependency; a lossy
 * or inter-frame codec can be added later as another choice without changing
 * the plane's shape. */

#define KMX_MOTION_MAX_RECTS 64

typedef struct kmx_motion kmx_motion;

typedef struct {
    bool keyframe;
    size_t rects;
    size_t wire_bytes;
    size_t pixel_bytes;   /* what an uncompressed full frame would have cost */
} kmx_motion_info;

/* `bytes_per_second` is the ceiling this pane may occupy.  Zero means no
 * limit, which is only sensible on a local socket. */
kmx_result kmx_motion_create(kmx_motion **out, uint32_t bytes_per_second);
void kmx_motion_free(kmx_motion *motion);

/* Offer a frame.  `produced` is false when nothing changed, or when the rate
 * allowance is spent - which is a drop, not an error. */
kmx_result kmx_motion_offer(
    kmx_motion *motion,
    const void *rgb,
    int width,
    int height,
    uint64_t now_millis,
    kmx_buffer *out,
    bool *produced,
    kmx_motion_info *info
);

/* Force the next frame to be a keyframe: after a resize, or for a client that
 * has just attached and holds nothing. */
void kmx_motion_invalidate(kmx_motion *motion);
size_t kmx_motion_dropped(const kmx_motion *motion);

typedef struct kmx_motion_sink kmx_motion_sink;

kmx_result kmx_motion_sink_create(kmx_motion_sink **out);
void kmx_motion_sink_free(kmx_motion_sink *sink);
kmx_result kmx_motion_sink_apply(
    kmx_motion_sink *sink,
    const void *data,
    size_t size
);
const unsigned char *kmx_motion_sink_pixels(
    const kmx_motion_sink *sink,
    int *width,
    int *height
);

/* ---- client-side rendering --------------------------------------------- */

/* Turns a received grid into terminal output, emitting only what changed
 * since the last frame it drew.  The client holds this so a repaint costs a
 * few cells rather than a screen. */
typedef struct kmx_render kmx_render;

kmx_result kmx_render_create(kmx_render **out);
void kmx_render_free(kmx_render *render);
/* Append the escape sequences that bring a terminal showing the previously
 * rendered frame to `grid`. */
kmx_result kmx_render_frame(kmx_render *render, const kmx_grid *grid, kmx_buffer *out);
/* Forget what is on screen, so the next frame is drawn in full. */
void kmx_render_invalidate(kmx_render *render);

/* ---- predictive local echo --------------------------------------------- */

/* Typing feels local only if it is shown before the round trip completes.
 * This predicts where typed characters will appear and withdraws the
 * prediction when the server's own version of the screen arrives.
 *
 * Predictions are epoch-gated: anything that invalidates the assumption that
 * typing appends at the cursor - a resize, a control character, a server frame
 * that disagrees - clears them rather than letting them drift. */
typedef struct kmx_predictor kmx_predictor;

kmx_result kmx_predictor_create(kmx_predictor **out);
void kmx_predictor_free(kmx_predictor *predictor);
/* Note locally typed bytes.  Returns true when the prediction changed what
 * should be on screen, so the caller knows to redraw. */
bool kmx_predictor_type(kmx_predictor *predictor, const void *data, size_t size);
/* Overlay outstanding predictions onto a server frame. */
void kmx_predictor_overlay(kmx_predictor *predictor, kmx_grid *grid);
/* A server frame arrived: confirm or withdraw predictions against it. */
void kmx_predictor_reconcile(kmx_predictor *predictor, const kmx_grid *grid);
void kmx_predictor_reset(kmx_predictor *predictor);
size_t kmx_predictor_outstanding(const kmx_predictor *predictor);

#ifdef __cplusplus
}
#endif

#endif
