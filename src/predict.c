/* Predictive local echo.
 *
 * On a 200 ms link, waiting for the server's version of the screen before
 * showing a keystroke makes typing feel broken.  So a typed character is drawn
 * immediately, where it is expected to land, and withdrawn when the server's
 * own frame arrives and either confirms or contradicts it.
 *
 * The part that is easy to get wrong is knowing when NOT to predict.  Typing
 * only appends at the cursor while the pane is doing something ordinary; a
 * control character, a resize, or a server frame that disagrees all break that
 * assumption.  Predictions are therefore epoch-gated: anything surprising
 * clears them rather than letting them drift out of step with reality.
 *
 * The idea is mosh's; its implementation is GPL and was not used. */
#include "kilix_mux.h"

#include <stdlib.h>
#include <string.h>

#define KMX_PREDICTION_MAX 64

typedef struct {
    int row;
    int col;
    uint32_t codepoint;
} prediction;

struct kmx_predictor {
    prediction pending[KMX_PREDICTION_MAX];
    size_t count;
    /* Where the next predicted character is expected to land. */
    int row;
    int col;
    bool anchored;
};

kmx_result
kmx_predictor_create(kmx_predictor **out) {
    kmx_predictor *predictor;
    if (!out) return KMX_ERR_INVALID;
    predictor = calloc(1, sizeof *predictor);
    if (!predictor) return KMX_ERR_MEMORY;
    *out = predictor;
    return KMX_OK;
}

void
kmx_predictor_free(kmx_predictor *predictor) {
    free(predictor);
}

void
kmx_predictor_reset(kmx_predictor *predictor) {
    if (!predictor) return;
    predictor->count = 0;
    predictor->anchored = false;
}

size_t
kmx_predictor_outstanding(const kmx_predictor *predictor) {
    return predictor ? predictor->count : 0;
}

bool
kmx_predictor_type(kmx_predictor *predictor, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t index;
    bool changed = false;
    if (!predictor || !bytes) return false;
    if (!predictor->anchored) return false;

    for (index = 0; index < size; index++) {
        unsigned char byte = bytes[index];
        /* Only plain printable ASCII is predicted.  Anything else - a control
         * character, an escape sequence, the start of a multi-byte sequence -
         * could move the cursor or change modes in ways this cannot model, so
         * the honest response is to stop predicting and wait for the server. */
        if (byte < 0x20 || byte >= 0x7f) {
            kmx_predictor_reset(predictor);
            return true;
        }
        if (predictor->count >= KMX_PREDICTION_MAX) {
            /* Further ahead than this and the guess is not worth trusting. */
            kmx_predictor_reset(predictor);
            return true;
        }
        predictor->pending[predictor->count].row = predictor->row;
        predictor->pending[predictor->count].col = predictor->col;
        predictor->pending[predictor->count].codepoint = byte;
        predictor->count++;
        predictor->col++;
        changed = true;
    }
    return changed;
}

void
kmx_predictor_overlay(kmx_predictor *predictor, kmx_grid *grid) {
    size_t index;
    if (!predictor || !grid) return;
    for (index = 0; index < predictor->count; index++) {
        const prediction *item = &predictor->pending[index];
        kmx_cell *cell = kmx_grid_cell(grid, item->row, item->col);
        if (!cell) continue;
        memset(cell->chars, 0, sizeof cell->chars);
        cell->chars[0] = item->codepoint;
        cell->width = 1;
        /* Underlined so an unconfirmed guess is visibly a guess. */
        cell->attrs |= KMX_ATTR_UNDERLINE;
        cell->underline = 1;
    }
    if (predictor->count) {
        const prediction *last = &predictor->pending[predictor->count - 1];
        if (last->col + 1 < grid->cols) {
            grid->cursor_row = last->row;
            grid->cursor_col = last->col + 1;
        }
    }
}

void
kmx_predictor_reconcile(kmx_predictor *predictor, const kmx_grid *grid) {
    size_t index;
    size_t kept = 0;
    if (!predictor || !grid) return;

    /* Anchor future predictions to wherever the server says the cursor is. */
    predictor->row = grid->cursor_row;
    predictor->col = grid->cursor_col;
    predictor->anchored = true;

    for (index = 0; index < predictor->count; index++) {
        const prediction *item = &predictor->pending[index];
        const kmx_cell *cell = kmx_grid_cell_const(grid, item->row, item->col);
        if (!cell) continue;
        if (cell->chars[0] == item->codepoint) {
            /* The server agrees; the prediction has served its purpose. */
            continue;
        }
        if (cell->chars[0] != 0 && cell->chars[0] != ' ') {
            /* The server put something else there.  The guess was wrong, and
             * every guess after it was built on the same assumption. */
            predictor->count = 0;
            return;
        }
        /* Still blank: the server has not caught up yet, so keep waiting. */
        predictor->pending[kept++] = *item;
    }
    predictor->count = kept;
    if (kept) {
        /* Continue from the far end of what is still outstanding. */
        const prediction *last = &predictor->pending[kept - 1];
        predictor->row = last->row;
        predictor->col = last->col + 1;
    }
}
