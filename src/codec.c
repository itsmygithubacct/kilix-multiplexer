/* Cell-plane codec.
 *
 * The wire form is a diff against a state the peer is known to hold, not a log
 * of what happened.  That is the property that makes a bad link cheap: a peer
 * that missed a megabyte of scrollback is brought current by one message
 * describing the screen it should now have, and a lost message is superseded
 * rather than retransmitted.
 *
 * Everything here is bounds-checked on decode.  This is the parser that faces
 * the network, so it is written to be fuzzed. */
#include "kilix_mux.h"

#include <string.h>

#define KMX_CELLS_MAGIC 0x4b4d5831u /* "KMX1" */

/* ---- varints ---------------------------------------------------------- */

static kmx_result
put_varint(kmx_buffer *out, uint64_t value) {
    unsigned char scratch[10];
    size_t used = 0;
    do {
        unsigned char byte = (unsigned char)(value & 0x7fu);
        value >>= 7;
        if (value) byte |= 0x80u;
        scratch[used++] = byte;
    } while (value);
    return kmx_buffer_append(out, scratch, used);
}

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
} reader;

static kmx_result
get_varint(reader *in, uint64_t *value) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        unsigned char byte;
        if (in->offset >= in->size) return KMX_ERR_TRUNCATED;
        byte = in->data[in->offset++];
        if (shift > 63) return KMX_ERR_PROTOCOL;
        result |= (uint64_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) break;
        shift += 7;
    }
    *value = result;
    return KMX_OK;
}

static kmx_result
get_byte(reader *in, unsigned char *value) {
    if (in->offset >= in->size) return KMX_ERR_TRUNCATED;
    *value = in->data[in->offset++];
    return KMX_OK;
}

/* Decoded lengths are used to size loops, so anything that cannot possibly be
 * a legal dimension is rejected before it becomes a loop bound. */
static kmx_result
get_bounded(reader *in, uint64_t limit, uint64_t *value) {
    kmx_result result = get_varint(in, value);
    if (result != KMX_OK) return result;
    return *value <= limit ? KMX_OK : KMX_ERR_PROTOCOL;
}

/* ---- colour ----------------------------------------------------------- */

static kmx_result
put_color(kmx_buffer *out, const kmx_color *color) {
    unsigned char scratch[4];
    scratch[0] = color->kind;
    scratch[1] = color->r;
    scratch[2] = color->g;
    scratch[3] = color->b;
    return kmx_buffer_append(out, scratch, sizeof scratch);
}

static kmx_result
get_color(reader *in, kmx_color *color) {
    if (in->offset + 4 > in->size) return KMX_ERR_TRUNCATED;
    color->kind = in->data[in->offset++];
    color->r = in->data[in->offset++];
    color->g = in->data[in->offset++];
    color->b = in->data[in->offset++];
    if (color->kind > KMX_COLOR_RGB) return KMX_ERR_PROTOCOL;
    return KMX_OK;
}

/* ---- runs ------------------------------------------------------------- */

/* Cells join one run when everything except their characters matches, so the
 * per-cell cost of a uniformly styled line is just its text. */
static bool
same_style(const kmx_cell *a, const kmx_cell *b) {
    return a->attrs == b->attrs && a->width == b->width &&
        a->underline == b->underline &&
        memcmp(&a->fg, &b->fg, sizeof a->fg) == 0 &&
        memcmp(&a->bg, &b->bg, sizeof a->bg) == 0;
}

static size_t
cell_char_count(const kmx_cell *cell) {
    size_t count = 0;
    while (count < KMX_MAX_CHARS && cell->chars[count]) count++;
    return count;
}

static kmx_result
put_row(kmx_buffer *out, const kmx_grid *grid, int row) {
    kmx_buffer runs;
    uint64_t run_count = 0;
    int col = 0;
    kmx_result result = KMX_OK;

    kmx_buffer_init(&runs);
    while (col < grid->cols) {
        const kmx_cell *first = kmx_grid_cell_const(grid, row, col);
        int end = col + 1;
        int index;
        while (end < grid->cols &&
               same_style(first, kmx_grid_cell_const(grid, row, end))) {
            end++;
        }
        result = put_varint(&runs, (uint64_t)col);
        if (result == KMX_OK) result = put_varint(&runs, (uint64_t)(end - col));
        if (result == KMX_OK) result = put_varint(&runs, first->attrs);
        if (result == KMX_OK) {
            unsigned char scratch[2] = {first->underline, first->width};
            result = kmx_buffer_append(&runs, scratch, sizeof scratch);
        }
        if (result == KMX_OK) result = put_color(&runs, &first->fg);
        if (result == KMX_OK) result = put_color(&runs, &first->bg);
        for (index = col; index < end && result == KMX_OK; index++) {
            const kmx_cell *cell = kmx_grid_cell_const(grid, row, index);
            size_t chars = cell_char_count(cell);
            size_t position;
            result = put_varint(&runs, chars);
            for (position = 0; position < chars && result == KMX_OK; position++) {
                result = put_varint(&runs, cell->chars[position]);
            }
        }
        if (result != KMX_OK) break;
        run_count++;
        col = end;
    }
    if (result == KMX_OK) result = put_varint(out, (uint64_t)row);
    if (result == KMX_OK) result = put_varint(out, run_count);
    if (result == KMX_OK) result = kmx_buffer_append(out, runs.data, runs.size);
    kmx_buffer_free(&runs);
    return result;
}

static bool
row_changed(const kmx_grid *previous, const kmx_grid *current, int row) {
    int col;
    if (!previous || previous->rows != current->rows ||
        previous->cols != current->cols) {
        return true;
    }
    for (col = 0; col < current->cols; col++) {
        if (!kmx_cell_equal(
                kmx_grid_cell_const(previous, row, col),
                kmx_grid_cell_const(current, row, col))) {
            return true;
        }
    }
    return false;
}

kmx_result
kmx_cells_encode(
    const kmx_grid *previous,
    const kmx_grid *current,
    kmx_buffer *out
) {
    kmx_buffer rows;
    uint64_t changed = 0;
    unsigned char header[4];
    unsigned char flags;
    int row;
    kmx_result result;

    if (!current || !out) return KMX_ERR_INVALID;
    if (previous == current) return KMX_ERR_INVALID;

    kmx_buffer_init(&rows);
    result = KMX_OK;
    for (row = 0; row < current->rows && result == KMX_OK; row++) {
        if (!row_changed(previous, current, row)) continue;
        result = put_row(&rows, current, row);
        if (result == KMX_OK) changed++;
    }
    if (result != KMX_OK) {
        kmx_buffer_free(&rows);
        return result;
    }

    header[0] = (unsigned char)(KMX_CELLS_MAGIC >> 24);
    header[1] = (unsigned char)(KMX_CELLS_MAGIC >> 16);
    header[2] = (unsigned char)(KMX_CELLS_MAGIC >> 8);
    header[3] = (unsigned char)KMX_CELLS_MAGIC;
    flags = current->cursor_visible ? 1u : 0u;

    result = kmx_buffer_append(out, header, sizeof header);
    if (result == KMX_OK) result = put_varint(out, (uint64_t)current->rows);
    if (result == KMX_OK) result = put_varint(out, (uint64_t)current->cols);
    if (result == KMX_OK) result = put_varint(out, (uint64_t)current->cursor_row);
    if (result == KMX_OK) result = put_varint(out, (uint64_t)current->cursor_col);
    if (result == KMX_OK) result = kmx_buffer_append(out, &flags, 1);
    if (result == KMX_OK) result = put_varint(out, changed);
    if (result == KMX_OK) result = kmx_buffer_append(out, rows.data, rows.size);
    kmx_buffer_free(&rows);
    return result;
}

kmx_result
kmx_cells_apply(kmx_grid *grid, const void *data, size_t size) {
    reader in;
    uint64_t rows;
    uint64_t cols;
    uint64_t cursor_row;
    uint64_t cursor_col;
    uint64_t changed;
    uint64_t index;
    unsigned char flags;
    kmx_result result;

    if (!grid || (!data && size)) return KMX_ERR_INVALID;
    in.data = data;
    in.size = size;
    in.offset = 0;

    if (size < 4) return KMX_ERR_TRUNCATED;
    if (((uint32_t)in.data[0] << 24 | (uint32_t)in.data[1] << 16 |
         (uint32_t)in.data[2] << 8 | (uint32_t)in.data[3]) != KMX_CELLS_MAGIC) {
        return KMX_ERR_PROTOCOL;
    }
    in.offset = 4;

    result = get_bounded(&in, KMX_MAX_DIMENSION, &rows);
    if (result == KMX_OK) result = get_bounded(&in, KMX_MAX_DIMENSION, &cols);
    if (result != KMX_OK) return result;
    if (!rows || !cols) return KMX_ERR_PROTOCOL;
    result = get_varint(&in, &cursor_row);
    if (result == KMX_OK) result = get_varint(&in, &cursor_col);
    if (result == KMX_OK) result = get_byte(&in, &flags);
    if (result != KMX_OK) return result;
    if (cursor_row >= rows || cursor_col >= cols) return KMX_ERR_PROTOCOL;

    result = kmx_grid_resize(grid, (int)rows, (int)cols);
    if (result != KMX_OK) return result;

    result = get_bounded(&in, rows, &changed);
    if (result != KMX_OK) return result;

    for (index = 0; index < changed; index++) {
        uint64_t row;
        uint64_t run_count;
        uint64_t run;
        result = get_bounded(&in, rows - 1, &row);
        if (result != KMX_OK) return result;
        result = get_bounded(&in, cols, &run_count);
        if (result != KMX_OK) return result;
        for (run = 0; run < run_count; run++) {
            uint64_t col;
            uint64_t length;
            uint64_t attrs;
            unsigned char underline;
            unsigned char width;
            kmx_color fg;
            kmx_color bg;
            uint64_t position;
            result = get_bounded(&in, cols - 1, &col);
            if (result == KMX_OK) result = get_bounded(&in, cols - col, &length);
            if (result == KMX_OK) result = get_bounded(&in, 0xffff, &attrs);
            if (result == KMX_OK) result = get_byte(&in, &underline);
            if (result == KMX_OK) result = get_byte(&in, &width);
            if (result == KMX_OK) result = get_color(&in, &fg);
            if (result == KMX_OK) result = get_color(&in, &bg);
            if (result != KMX_OK) return result;
            if (!length) return KMX_ERR_PROTOCOL;
            for (position = 0; position < length; position++) {
                kmx_cell *cell = kmx_grid_cell(grid, (int)row, (int)(col + position));
                uint64_t chars;
                uint64_t character;
                if (!cell) return KMX_ERR_PROTOCOL;
                result = get_bounded(&in, KMX_MAX_CHARS, &chars);
                if (result != KMX_OK) return result;
                memset(cell, 0, sizeof *cell);
                cell->attrs = (uint16_t)attrs;
                cell->underline = underline;
                cell->width = width;
                cell->fg = fg;
                cell->bg = bg;
                for (character = 0; character < chars; character++) {
                    uint64_t codepoint;
                    result = get_varint(&in, &codepoint);
                    if (result != KMX_OK) return result;
                    if (codepoint > 0x10ffff) return KMX_ERR_PROTOCOL;
                    cell->chars[character] = (uint32_t)codepoint;
                }
            }
        }
    }
    grid->cursor_row = (int)cursor_row;
    grid->cursor_col = (int)cursor_col;
    grid->cursor_visible = (flags & 1u) != 0;
    /* Trailing bytes mean the sender and receiver disagree about the format,
     * which is worth failing on rather than ignoring. */
    return in.offset == in.size ? KMX_OK : KMX_ERR_PROTOCOL;
}
