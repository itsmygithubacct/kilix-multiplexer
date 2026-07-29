/* The layout plane.
 *
 * A few dozen bytes describing where the panes are, sent when the arrangement
 * changes and not otherwise.  This is the cheapest plane and the one that
 * saves the most: because the client knows the geometry, it draws its own
 * dividers and titles rather than being sent pictures of them.  A pixel
 * protocol re-encodes that chrome sixty times a second; here it costs nothing
 * at all while the arrangement holds still. */
#define _POSIX_C_SOURCE 200809L

#include "kilix_mux.h"

#include <string.h>

void
kmx_layout_init(kmx_layout *layout, int rows, int cols) {
    if (!layout) return;
    memset(layout, 0, sizeof *layout);
    layout->rows = rows;
    layout->cols = cols;
}

const kmx_pane_info *
kmx_layout_find(const kmx_layout *layout, uint32_t id) {
    size_t index;
    if (!layout) return NULL;
    for (index = 0; index < layout->pane_count; index++) {
        if (layout->panes[index].id == id) return &layout->panes[index];
    }
    return NULL;
}

bool
kmx_layout_equal(const kmx_layout *a, const kmx_layout *b) {
    size_t index;
    if (!a || !b) return a == b;
    if (a->rows != b->rows || a->cols != b->cols) return false;
    if (a->pane_count != b->pane_count) return false;
    for (index = 0; index < a->pane_count; index++) {
        const kmx_pane_info *left = &a->panes[index];
        const kmx_pane_info *right = &b->panes[index];
        if (left->id != right->id || left->x != right->x || left->y != right->y ||
            left->rows != right->rows || left->cols != right->cols ||
            left->focused != right->focused) {
            return false;
        }
        if (strncmp(left->title, right->title, KMX_TITLE_MAX) != 0) return false;
    }
    return true;
}

/* Evenly split the screen, giving the remainder to the earlier panes so the
 * total is always exactly the screen and never a row short. */
kmx_result
kmx_layout_arrange(kmx_layout *layout, size_t count, bool vertical) {
    size_t index;
    int used = 0;
    if (!layout || !count || count > KMX_MAX_PANES) return KMX_ERR_INVALID;
    /* One divider column or row between neighbours, and a title row per pane
     * once there is more than one pane to tell apart. */
    if (vertical) {
        int available = layout->rows - (int)(count - 1);
        if (available < (int)count * 2) return KMX_ERR_LIMIT;
    } else {
        int available = layout->cols - (int)(count - 1);
        if (available < (int)count * 4) return KMX_ERR_LIMIT;
    }

    layout->pane_count = count;
    for (index = 0; index < count; index++) {
        kmx_pane_info *pane = &layout->panes[index];
        size_t remaining = count - index;
        if (!pane->id) pane->id = (uint32_t)(index + 1);
        pane->focused = index == 0;
        if (vertical) {
            int span = (layout->rows - used - (int)(remaining - 1)) / (int)remaining;
            pane->x = 0;
            pane->y = used;
            pane->cols = layout->cols;
            pane->rows = span;
            used += span + 1;
        } else {
            int span = (layout->cols - used - (int)(remaining - 1)) / (int)remaining;
            pane->x = used;
            pane->y = 0;
            pane->rows = layout->rows;
            pane->cols = span;
            used += span + 1;
        }
        /* When panes share the screen the top row of each is its title, so
         * the pane's usable area is one row shorter. */
        if (count > 1 && vertical) {
            pane->y += 1;
            pane->rows -= 1;
        } else if (count > 1) {
            pane->y += 1;
            pane->rows -= 1;
        }
        if (pane->rows < 1) pane->rows = 1;
        if (pane->cols < 1) pane->cols = 1;
    }
    layout->generation++;
    return KMX_OK;
}

/* ---- wire form -------------------------------------------------------- */

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
get_bounded(reader *in, uint64_t limit, uint64_t *value) {
    kmx_result result = get_varint(in, value);
    if (result != KMX_OK) return result;
    return *value <= limit ? KMX_OK : KMX_ERR_PROTOCOL;
}

kmx_result
kmx_layout_encode(const kmx_layout *layout, kmx_buffer *out) {
    size_t index;
    kmx_result result;
    if (!layout || !out) return KMX_ERR_INVALID;
    result = put_varint(out, (uint64_t)layout->rows);
    if (result == KMX_OK) result = put_varint(out, (uint64_t)layout->cols);
    if (result == KMX_OK) result = put_varint(out, layout->generation);
    if (result == KMX_OK) result = put_varint(out, layout->pane_count);
    for (index = 0; index < layout->pane_count && result == KMX_OK; index++) {
        const kmx_pane_info *pane = &layout->panes[index];
        size_t title = strnlen(pane->title, KMX_TITLE_MAX);
        unsigned char focused = pane->focused ? 1u : 0u;
        result = put_varint(out, pane->id);
        if (result == KMX_OK) result = put_varint(out, (uint64_t)pane->x);
        if (result == KMX_OK) result = put_varint(out, (uint64_t)pane->y);
        if (result == KMX_OK) result = put_varint(out, (uint64_t)pane->rows);
        if (result == KMX_OK) result = put_varint(out, (uint64_t)pane->cols);
        if (result == KMX_OK) result = kmx_buffer_append(out, &focused, 1);
        if (result == KMX_OK) result = put_varint(out, title);
        if (result == KMX_OK) result = kmx_buffer_append(out, pane->title, title);
    }
    return result;
}

kmx_result
kmx_layout_apply(kmx_layout *layout, const void *data, size_t size) {
    reader in;
    uint64_t rows;
    uint64_t cols;
    uint64_t generation;
    uint64_t count;
    uint64_t index;
    kmx_result result;

    if (!layout || (!data && size)) return KMX_ERR_INVALID;
    in.data = data;
    in.size = size;
    in.offset = 0;

    result = get_bounded(&in, KMX_MAX_DIMENSION, &rows);
    if (result == KMX_OK) result = get_bounded(&in, KMX_MAX_DIMENSION, &cols);
    if (result == KMX_OK) result = get_varint(&in, &generation);
    if (result == KMX_OK) result = get_bounded(&in, KMX_MAX_PANES, &count);
    if (result != KMX_OK) return result;
    if (!rows || !cols) return KMX_ERR_PROTOCOL;

    memset(layout, 0, sizeof *layout);
    layout->rows = (int32_t)rows;
    layout->cols = (int32_t)cols;
    layout->generation = (uint32_t)generation;
    layout->pane_count = (size_t)count;

    for (index = 0; index < count; index++) {
        kmx_pane_info *pane = &layout->panes[index];
        uint64_t id;
        uint64_t x;
        uint64_t y;
        uint64_t pane_rows;
        uint64_t pane_cols;
        uint64_t title;
        unsigned char focused;
        result = get_varint(&in, &id);
        if (result == KMX_OK) result = get_bounded(&in, cols - 1, &x);
        if (result == KMX_OK) result = get_bounded(&in, rows - 1, &y);
        if (result == KMX_OK) result = get_bounded(&in, rows, &pane_rows);
        if (result == KMX_OK) result = get_bounded(&in, cols, &pane_cols);
        if (result != KMX_OK) return result;
        if (in.offset >= in.size) return KMX_ERR_TRUNCATED;
        focused = in.data[in.offset++];
        result = get_bounded(&in, KMX_TITLE_MAX - 1, &title);
        if (result != KMX_OK) return result;
        if (in.size - in.offset < title) return KMX_ERR_TRUNCATED;
        /* A pane must fit inside the screen it claims to be part of. */
        if (x + pane_cols > cols || y + pane_rows > rows) return KMX_ERR_PROTOCOL;
        if (!pane_rows || !pane_cols) return KMX_ERR_PROTOCOL;
        pane->id = (uint32_t)id;
        pane->x = (int32_t)x;
        pane->y = (int32_t)y;
        pane->rows = (int32_t)pane_rows;
        pane->cols = (int32_t)pane_cols;
        pane->focused = focused != 0;
        memcpy(pane->title, in.data + in.offset, (size_t)title);
        pane->title[title] = '\0';
        in.offset += (size_t)title;
    }
    return in.offset == in.size ? KMX_OK : KMX_ERR_PROTOCOL;
}

/* ---- compositing ------------------------------------------------------ */

static void
put_char(kmx_grid *grid, int row, int col, uint32_t codepoint, uint16_t attrs) {
    kmx_cell *cell = kmx_grid_cell(grid, row, col);
    if (!cell) return;
    memset(cell, 0, sizeof *cell);
    cell->chars[0] = codepoint;
    cell->width = 1;
    cell->attrs = attrs;
}

kmx_result
kmx_layout_composite(
    const kmx_layout *layout,
    const kmx_grid *const *panes,
    size_t pane_count,
    kmx_grid *out
) {
    size_t index;
    kmx_result result;
    if (!layout || !out || (!panes && pane_count)) return KMX_ERR_INVALID;
    result = out->cells ? kmx_grid_resize(out, layout->rows, layout->cols)
                        : kmx_grid_init(out, layout->rows, layout->cols);
    if (result != KMX_OK) return result;
    memset(
        out->cells, 0,
        (size_t)out->rows * (size_t)out->cols * sizeof *out->cells);

    for (index = 0; index < layout->pane_count; index++) {
        const kmx_pane_info *pane = &layout->panes[index];
        const kmx_grid *source = index < pane_count ? panes[index] : NULL;
        int row;

        /* The title row, drawn by the client from the layout rather than
         * received as content.  Reversed for the focused pane so which one
         * takes typing is visible without a cursor hunt. */
        if (layout->pane_count > 1 && pane->y > 0) {
            const char *title = pane->title[0] ? pane->title : "pane";
            size_t length = strnlen(title, KMX_TITLE_MAX);
            uint16_t attrs = pane->focused ? KMX_ATTR_REVERSE : KMX_ATTR_BOLD;
            int col;
            /* The title row is padded to the pane's full width so the
             * highlight reads as a bar rather than as a word. */
            for (col = 0; col < pane->cols; col++) {
                uint32_t letter = (size_t)col < length
                    ? (uint32_t)(unsigned char)title[col] : (uint32_t)' ';
                put_char(out, pane->y - 1, pane->x + col, letter, attrs);
            }
        }

        /* The divider column between side-by-side panes. */
        if (pane->x + pane->cols < layout->cols) {
            int divider_row;
            for (divider_row = 0; divider_row < layout->rows; divider_row++) {
                put_char(out, divider_row, pane->x + pane->cols, 0x2502, 0);
            }
        }

        if (!source) continue;
        for (row = 0; row < pane->rows && row < source->rows; row++) {
            int col;
            for (col = 0; col < pane->cols && col < source->cols; col++) {
                kmx_cell *target = kmx_grid_cell(out, pane->y + row, pane->x + col);
                const kmx_cell *cell = kmx_grid_cell_const(source, row, col);
                if (target && cell) *target = *cell;
            }
        }
        if (pane->focused && source) {
            out->cursor_row = pane->y + source->cursor_row;
            out->cursor_col = pane->x + source->cursor_col;
            out->cursor_visible = source->cursor_visible;
        }
    }
    return KMX_OK;
}
