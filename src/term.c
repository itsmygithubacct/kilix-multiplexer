/* Server-side terminal model.
 *
 * The multiplexer parses the pane's output a second time, independently of the
 * terminal that is also rendering it, because that is what lets it answer
 * "what is on screen now" rather than "what happened".  The cost is one extra
 * parse per pane; the benefit is that a peer which missed everything can be
 * brought current in one message.
 *
 * Graphics escapes are collected here rather than discarded.  libvterm hands
 * them to the unrecognised-APC fallback in wire order relative to the cell
 * changes around them, so an image placement and the text beside it cannot be
 * reordered - which is the whole reason this layer parses rather than sniffs. */
#include "kilix_mux.h"

#include "vterm.h"

#include <stdlib.h>
#include <string.h>

#define KMX_GRAPHICS_MAX 256

struct kmx_term {
    VTerm *vt;
    VTermScreen *screen;
    /* libvterm stores the pointer it is handed, not a copy, so this must
     * outlive every call into the parser. */
    VTermStateFallbacks fallbacks;
    kmx_graphics_event graphics[KMX_GRAPHICS_MAX];
    size_t graphics_count;
    bool graphics_open;   /* a fragmented sequence is still arriving */
    bool graphics_lost;   /* the queue overflowed; do not fabricate order */
    size_t stream_offset;
};

static kmx_color
from_vterm_color(const VTermColor *color) {
    kmx_color out;
    memset(&out, 0, sizeof out);
    if (VTERM_COLOR_IS_DEFAULT_FG(color) || VTERM_COLOR_IS_DEFAULT_BG(color)) {
        out.kind = KMX_COLOR_DEFAULT;
        return out;
    }
    if (VTERM_COLOR_IS_INDEXED(color)) {
        out.kind = KMX_COLOR_INDEXED;
        out.r = color->indexed.idx;
        return out;
    }
    out.kind = KMX_COLOR_RGB;
    out.r = color->rgb.red;
    out.g = color->rgb.green;
    out.b = color->rgb.blue;
    return out;
}

static int
on_apc(VTermStringFragment frag, void *user) {
    kmx_term *term = user;
    kmx_graphics_event *event;
    if (frag.initial) {
        if (term->graphics_count >= KMX_GRAPHICS_MAX) {
            /* Drop rather than grow without bound.  The flag is what stops a
             * consumer from believing it has a complete, ordered record. */
            term->graphics_lost = true;
            term->graphics_open = false;
            return 1;
        }
        event = &term->graphics[term->graphics_count++];
        kmx_buffer_init(&event->payload);
        event->offset = term->stream_offset;
        term->graphics_open = true;
    }
    if (!term->graphics_open || !term->graphics_count) return 1;
    event = &term->graphics[term->graphics_count - 1];
    if (frag.len &&
        kmx_buffer_append(&event->payload, frag.str, frag.len) != KMX_OK) {
        term->graphics_lost = true;
    }
    if (frag.final) term->graphics_open = false;
    return 1;
}

kmx_result
kmx_term_create(kmx_term **out, int rows, int cols) {
    kmx_term *term;
    if (!out || rows <= 0 || cols <= 0) return KMX_ERR_INVALID;
    term = calloc(1, sizeof *term);
    if (!term) return KMX_ERR_MEMORY;
    term->vt = vterm_new(rows, cols);
    if (!term->vt) {
        free(term);
        return KMX_ERR_MEMORY;
    }
    vterm_set_utf8(term->vt, 1);
    term->screen = vterm_obtain_screen(term->vt);
    if (!term->screen) {
        vterm_free(term->vt);
        free(term);
        return KMX_ERR_MEMORY;
    }
    memset(&term->fallbacks, 0, sizeof term->fallbacks);
    term->fallbacks.apc = on_apc;
    vterm_screen_set_unrecognised_fallbacks(term->screen, &term->fallbacks, term);
    vterm_screen_reset(term->screen, 1);
    *out = term;
    return KMX_OK;
}

void
kmx_term_free(kmx_term *term) {
    if (!term) return;
    kmx_term_graphics_clear(term);
    if (term->vt) vterm_free(term->vt);
    free(term);
}

kmx_result
kmx_term_feed(kmx_term *term, const void *data, size_t size) {
    if (!term || (!data && size)) return KMX_ERR_INVALID;
    if (!size) return KMX_OK;
    vterm_input_write(term->vt, data, size);
    vterm_screen_flush_damage(term->screen);
    term->stream_offset += size;
    return KMX_OK;
}

kmx_result
kmx_term_resize(kmx_term *term, int rows, int cols) {
    if (!term || rows <= 0 || cols <= 0) return KMX_ERR_INVALID;
    vterm_set_size(term->vt, rows, cols);
    vterm_screen_flush_damage(term->screen);
    return KMX_OK;
}

kmx_result
kmx_term_snapshot(kmx_term *term, kmx_grid *grid) {
    VTermPos cursor;
    VTermState *state;
    int rows;
    int cols;
    int row;
    int col;
    kmx_result result;

    if (!term || !grid) return KMX_ERR_INVALID;
    vterm_get_size(term->vt, &rows, &cols);
    result = grid->cells ? kmx_grid_resize(grid, rows, cols)
                         : kmx_grid_init(grid, rows, cols);
    if (result != KMX_OK) return result;

    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++) {
            VTermScreenCell source;
            kmx_cell *cell = kmx_grid_cell(grid, row, col);
            VTermPos position = {.row = row, .col = col};
            size_t index;
            memset(cell, 0, sizeof *cell);
            if (!vterm_screen_get_cell(term->screen, position, &source)) continue;
            /* libvterm marks the right half of a double-width character with a
             * (uint32_t)-1 sentinel in chars[0].  That is an internal
             * representation, not a codepoint, so it is normalised away here:
             * a continuation cell carries no characters and width 0.  Keeping
             * it off the wire means the decoder can reject anything that is
             * not a real codepoint. */
            if (source.chars[0] == (uint32_t)-1) {
                cell->width = 0;
                cell->underline = (uint8_t)source.attrs.underline;
                cell->fg = from_vterm_color(&source.fg);
                cell->bg = from_vterm_color(&source.bg);
                continue;
            }
            for (index = 0; index < KMX_MAX_CHARS && index < VTERM_MAX_CHARS_PER_CELL;
                 index++) {
                cell->chars[index] = source.chars[index];
                if (!source.chars[index]) break;
            }
            cell->width = (uint8_t)source.width;
            cell->underline = (uint8_t)source.attrs.underline;
            cell->attrs =
                (uint16_t)((source.attrs.bold ? KMX_ATTR_BOLD : 0) |
                           (source.attrs.underline ? KMX_ATTR_UNDERLINE : 0) |
                           (source.attrs.italic ? KMX_ATTR_ITALIC : 0) |
                           (source.attrs.blink ? KMX_ATTR_BLINK : 0) |
                           (source.attrs.reverse ? KMX_ATTR_REVERSE : 0) |
                           (source.attrs.conceal ? KMX_ATTR_CONCEAL : 0) |
                           (source.attrs.strike ? KMX_ATTR_STRIKE : 0) |
                           (source.attrs.dwl ? KMX_ATTR_DWL : 0) |
                           (source.attrs.dhl == 1 ? KMX_ATTR_DHL_TOP : 0) |
                           (source.attrs.dhl == 2 ? KMX_ATTR_DHL_BOTTOM : 0) |
                           (source.attrs.small ? KMX_ATTR_SMALL : 0) |
                           (source.attrs.baseline == 1 ? KMX_ATTR_BASELINE_RAISE : 0) |
                           (source.attrs.baseline == 2 ? KMX_ATTR_BASELINE_LOWER : 0));
            cell->fg = from_vterm_color(&source.fg);
            cell->bg = from_vterm_color(&source.bg);
        }
    }
    state = vterm_obtain_state(term->vt);
    vterm_state_get_cursorpos(state, &cursor);
    grid->cursor_row = cursor.row < 0 ? 0 : (cursor.row >= rows ? rows - 1 : cursor.row);
    grid->cursor_col = cursor.col < 0 ? 0 : (cursor.col >= cols ? cols - 1 : cursor.col);
    grid->cursor_visible = true;
    return KMX_OK;
}

size_t
kmx_term_graphics_count(const kmx_term *term) {
    return term ? term->graphics_count : 0;
}

const kmx_graphics_event *
kmx_term_graphics_at(const kmx_term *term, size_t index) {
    if (!term || index >= term->graphics_count) return NULL;
    return &term->graphics[index];
}

void
kmx_term_graphics_clear(kmx_term *term) {
    size_t index;
    if (!term) return;
    for (index = 0; index < term->graphics_count; index++) {
        kmx_buffer_free(&term->graphics[index].payload);
    }
    term->graphics_count = 0;
    term->graphics_open = false;
    term->graphics_lost = false;
}
