/* Turning a received grid back into terminal output.
 *
 * The client keeps the frame it last drew and emits only the difference, so a
 * screen that changed in one place costs one cell rather than a repaint.  That
 * matters even though the link is not involved: the local terminal is the
 * other place where a naive implementation spends its time. */
#include "kilix_mux.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct kmx_render {
    kmx_grid previous;
    bool valid;
};

kmx_result
kmx_render_create(kmx_render **out) {
    kmx_render *render;
    if (!out) return KMX_ERR_INVALID;
    render = calloc(1, sizeof *render);
    if (!render) return KMX_ERR_MEMORY;
    *out = render;
    return KMX_OK;
}

void
kmx_render_free(kmx_render *render) {
    if (!render) return;
    kmx_grid_free(&render->previous);
    free(render);
}

void
kmx_render_invalidate(kmx_render *render) {
    if (render) render->valid = false;
}

static kmx_result
append_text(kmx_buffer *out, const char *text) {
    return kmx_buffer_append(out, text, strlen(text));
}

static kmx_result
append_utf8(kmx_buffer *out, uint32_t codepoint) {
    unsigned char scratch[4];
    size_t used;
    if (codepoint < 0x80) {
        scratch[0] = (unsigned char)codepoint;
        used = 1;
    } else if (codepoint < 0x800) {
        scratch[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        scratch[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        used = 2;
    } else if (codepoint < 0x10000) {
        scratch[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        scratch[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        scratch[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        used = 3;
    } else if (codepoint <= 0x10ffff) {
        scratch[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        scratch[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        scratch[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        scratch[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
        used = 4;
    } else {
        return KMX_ERR_INVALID;
    }
    return kmx_buffer_append(out, scratch, used);
}

static kmx_result
append_color(kmx_buffer *out, const kmx_color *color, bool foreground) {
    char scratch[32];
    int written;
    switch (color->kind) {
        case KMX_COLOR_INDEXED:
            written = snprintf(
                scratch, sizeof scratch, "\033[%d;5;%um",
                foreground ? 38 : 48, color->r);
            break;
        case KMX_COLOR_RGB:
            written = snprintf(
                scratch, sizeof scratch, "\033[%d;2;%u;%u;%um",
                foreground ? 38 : 48, color->r, color->g, color->b);
            break;
        default:
            written = snprintf(
                scratch, sizeof scratch, "\033[%dm", foreground ? 39 : 49);
            break;
    }
    if (written < 0 || (size_t)written >= sizeof scratch) return KMX_ERR_INVALID;
    return kmx_buffer_append(out, scratch, (size_t)written);
}

/* Emit the style for `cell`.  Written as a reset followed by what is wanted,
 * rather than a minimal transition, because correctness across arbitrary
 * previous state is worth more here than a handful of bytes to a local
 * terminal. */
static kmx_result
append_style(kmx_buffer *out, const kmx_cell *cell) {
    kmx_result result = append_text(out, "\033[0m");
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_BOLD)) {
        result = append_text(out, "\033[1m");
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_ITALIC)) {
        result = append_text(out, "\033[3m");
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_UNDERLINE)) {
        char underline[16];
        unsigned style = cell->underline ? cell->underline : 1u;
        int written = snprintf(underline, sizeof underline, "\033[4:%um", style);
        if (written < 0 || (size_t)written >= sizeof underline) return KMX_ERR_INVALID;
        result = kmx_buffer_append(out, underline, (size_t)written);
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_BLINK)) {
        result = append_text(out, "\033[5m");
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_REVERSE)) {
        result = append_text(out, "\033[7m");
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_CONCEAL)) {
        result = append_text(out, "\033[8m");
    }
    if (result == KMX_OK && (cell->attrs & KMX_ATTR_STRIKE)) {
        result = append_text(out, "\033[9m");
    }
    if (result == KMX_OK) result = append_color(out, &cell->fg, true);
    if (result == KMX_OK) result = append_color(out, &cell->bg, false);
    return result;
}

static bool
same_style(const kmx_cell *a, const kmx_cell *b) {
    return a->attrs == b->attrs && a->underline == b->underline &&
        memcmp(&a->fg, &b->fg, sizeof a->fg) == 0 &&
        memcmp(&a->bg, &b->bg, sizeof a->bg) == 0;
}

static kmx_result
move_cursor(kmx_buffer *out, int row, int col) {
    char scratch[32];
    int written = snprintf(scratch, sizeof scratch, "\033[%d;%dH", row + 1, col + 1);
    if (written < 0 || (size_t)written >= sizeof scratch) return KMX_ERR_INVALID;
    return kmx_buffer_append(out, scratch, (size_t)written);
}

kmx_result
kmx_render_frame(kmx_render *render, const kmx_grid *grid, kmx_buffer *out) {
    bool full;
    int row;
    int cursor_row = -1;
    int cursor_col = -1;
    const kmx_cell *style = NULL;
    kmx_result result = KMX_OK;

    if (!render || !grid || !out) return KMX_ERR_INVALID;
    full = !render->valid ||
        render->previous.rows != grid->rows ||
        render->previous.cols != grid->cols;
    if (full) {
        result = append_text(out, "\033[H\033[2J");
        if (result != KMX_OK) return result;
    }

    for (row = 0; row < grid->rows && result == KMX_OK; row++) {
        int col;
        bool repaint_next = false;
        for (col = 0; col < grid->cols && result == KMX_OK; col++) {
            const kmx_cell *cell = kmx_grid_cell_const(grid, row, col);
            bool forced = repaint_next;
            bool unicode = false;
            repaint_next = false;
            /* The tail of a wide character is drawn by its head. */
            if (cell->width == 0) continue;
            if (!full && !forced) {
                const kmx_cell *before = kmx_grid_cell_const(&render->previous, row, col);
                if (before && kmx_cell_equal(before, cell)) continue;
            }
            if (cursor_row != row || cursor_col != col) {
                result = move_cursor(out, row, col);
                if (result != KMX_OK) break;
                cursor_row = row;
                cursor_col = col;
            }
            /* Cursor moves do not change SGR. Reuse a style within this
             * frame, including across untouched cells and row boundaries.
             * Each frame still establishes its first style independently. */
            if (!style || !same_style(style, cell)) {
                result = append_style(out, cell);
                if (result != KMX_OK) break;
                style = cell;
            }
            if (!cell->chars[0]) {
                result = kmx_buffer_append(out, " ", 1);
            } else {
                size_t index;
                for (index = 0; index < KMX_MAX_CHARS && cell->chars[index] &&
                     result == KMX_OK; index++) {
                    if (cell->chars[index] >= 0x80u) unicode = true;
                    result = append_utf8(out, cell->chars[index]);
                }
            }
            if (unicode || cell->width != 1) {
                /* Overwriting a wide character's tail leaves its head in the
                 * model with width 1, although printing that codepoint still
                 * advances the local terminal by 2. Reposition before the
                 * next write and restore the adjacent cell even when its
                 * value did not change: the glyph may just have erased it.
                 * This also avoids guessing widths for combining sequences. */
                cursor_col = -1;
                repaint_next = cell->width == 1;
            } else {
                cursor_col += cell->width ? cell->width : 1;
            }
        }
    }
    if (result == KMX_OK) result = append_text(out, "\033[0m");
    if (result == KMX_OK) {
        result = move_cursor(out, grid->cursor_row, grid->cursor_col);
    }
    if (result == KMX_OK) {
        result = append_text(out, grid->cursor_visible ? "\033[?25h" : "\033[?25l");
    }
    if (result == KMX_OK) {
        result = kmx_grid_copy(&render->previous, grid);
        if (result == KMX_OK) render->valid = true;
    }
    return result;
}
