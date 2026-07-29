#include "kilix_mux.h"

#include <stdlib.h>
#include <string.h>

const char *
kmx_result_string(kmx_result result) {
    switch (result) {
        case KMX_OK: return "success";
        case KMX_ERR_INVALID: return "invalid argument";
        case KMX_ERR_MEMORY: return "out of memory";
        case KMX_ERR_PROTOCOL: return "protocol error";
        case KMX_ERR_TRUNCATED: return "truncated message";
        case KMX_ERR_LIMIT: return "limit exceeded";
    }
    return "unknown error";
}

bool
kmx_cell_equal(const kmx_cell *a, const kmx_cell *b) {
    if (a->attrs != b->attrs || a->width != b->width ||
        a->underline != b->underline) {
        return false;
    }
    if (memcmp(&a->fg, &b->fg, sizeof a->fg) != 0) return false;
    if (memcmp(&a->bg, &b->bg, sizeof a->bg) != 0) return false;
    return memcmp(a->chars, b->chars, sizeof a->chars) == 0;
}

static bool
valid_dimensions(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return false;
    if (rows > KMX_MAX_DIMENSION || cols > KMX_MAX_DIMENSION) return false;
    return (long long)rows * (long long)cols <= (long long)KMX_MAX_CELLS;
}

kmx_result
kmx_grid_init(kmx_grid *grid, int rows, int cols) {
    if (!grid || !valid_dimensions(rows, cols)) return KMX_ERR_INVALID;
    memset(grid, 0, sizeof *grid);
    grid->cells = calloc((size_t)rows * (size_t)cols, sizeof *grid->cells);
    if (!grid->cells) return KMX_ERR_MEMORY;
    grid->rows = rows;
    grid->cols = cols;
    grid->cursor_visible = true;
    return KMX_OK;
}

void
kmx_grid_free(kmx_grid *grid) {
    if (!grid) return;
    free(grid->cells);
    memset(grid, 0, sizeof *grid);
}

kmx_result
kmx_grid_resize(kmx_grid *grid, int rows, int cols) {
    kmx_cell *replacement;
    if (!grid || !valid_dimensions(rows, cols)) return KMX_ERR_INVALID;
    if (grid->rows == rows && grid->cols == cols) return KMX_OK;
    replacement = calloc((size_t)rows * (size_t)cols, sizeof *replacement);
    if (!replacement) return KMX_ERR_MEMORY;
    free(grid->cells);
    grid->cells = replacement;
    grid->rows = rows;
    grid->cols = cols;
    if (grid->cursor_row >= rows) grid->cursor_row = rows - 1;
    if (grid->cursor_col >= cols) grid->cursor_col = cols - 1;
    return KMX_OK;
}

kmx_result
kmx_grid_copy(kmx_grid *destination, const kmx_grid *source) {
    kmx_result result;
    if (!destination || !source) return KMX_ERR_INVALID;
    result = kmx_grid_resize(destination, source->rows, source->cols);
    if (result != KMX_OK) return result;
    memcpy(
        destination->cells, source->cells,
        (size_t)source->rows * (size_t)source->cols * sizeof *source->cells);
    destination->cursor_row = source->cursor_row;
    destination->cursor_col = source->cursor_col;
    destination->cursor_visible = source->cursor_visible;
    return KMX_OK;
}

bool
kmx_grid_equal(const kmx_grid *a, const kmx_grid *b) {
    size_t count;
    size_t index;
    if (!a || !b) return a == b;
    if (a->rows != b->rows || a->cols != b->cols) return false;
    if (a->cursor_row != b->cursor_row || a->cursor_col != b->cursor_col) return false;
    if (a->cursor_visible != b->cursor_visible) return false;
    count = (size_t)a->rows * (size_t)a->cols;
    for (index = 0; index < count; index++) {
        if (!kmx_cell_equal(&a->cells[index], &b->cells[index])) return false;
    }
    return true;
}

kmx_cell *
kmx_grid_cell(kmx_grid *grid, int row, int col) {
    if (!grid || row < 0 || col < 0 || row >= grid->rows || col >= grid->cols) {
        return NULL;
    }
    return &grid->cells[(size_t)row * (size_t)grid->cols + (size_t)col];
}

const kmx_cell *
kmx_grid_cell_const(const kmx_grid *grid, int row, int col) {
    return kmx_grid_cell((kmx_grid *)grid, row, col);
}

/* ---- buffer ----------------------------------------------------------- */

void
kmx_buffer_init(kmx_buffer *buffer) {
    if (buffer) memset(buffer, 0, sizeof *buffer);
}

void
kmx_buffer_free(kmx_buffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof *buffer);
}

void
kmx_buffer_reset(kmx_buffer *buffer) {
    if (buffer) buffer->size = 0;
}

kmx_result
kmx_buffer_append(kmx_buffer *buffer, const void *data, size_t size) {
    if (!buffer || (!data && size)) return KMX_ERR_INVALID;
    if (!size) return KMX_OK;
    if (buffer->size + size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        unsigned char *replacement;
        while (capacity < buffer->size + size) {
            if (capacity > (size_t)-1 / 2) return KMX_ERR_MEMORY;
            capacity *= 2;
        }
        replacement = realloc(buffer->data, capacity);
        if (!replacement) return KMX_ERR_MEMORY;
        buffer->data = replacement;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return KMX_OK;
}
