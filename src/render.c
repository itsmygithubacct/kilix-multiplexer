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
        result = append_text(out, "\033[4m");
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
        for (col = 0; col < grid->cols && result == KMX_OK; col++) {
            const kmx_cell *cell = kmx_grid_cell_const(grid, row, col);
            /* The tail of a wide character is drawn by its head. */
            if (cell->width == 0) continue;
            if (!full) {
                const kmx_cell *before = kmx_grid_cell_const(&render->previous, row, col);
                if (before && kmx_cell_equal(before, cell)) continue;
            }
            if (cursor_row != row || cursor_col != col) {
                result = move_cursor(out, row, col);
                if (result != KMX_OK) break;
                cursor_row = row;
                cursor_col = col;
            }
            result = append_style(out, cell);
            if (result != KMX_OK) break;
            if (!cell->chars[0]) {
                result = kmx_buffer_append(out, " ", 1);
            } else {
                size_t index;
                for (index = 0; index < KMX_MAX_CHARS && cell->chars[index] &&
                     result == KMX_OK; index++) {
                    result = append_utf8(out, cell->chars[index]);
                }
            }
            cursor_col += cell->width ? cell->width : 1;
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
