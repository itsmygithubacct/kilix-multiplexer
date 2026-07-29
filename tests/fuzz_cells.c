/* libFuzzer target for the cell-plane decoder.
 *
 * This is the parser a remote peer feeds directly, so it is the one place in
 * the project where "it passed the tests" is not a sufficient argument.  The
 * decoder may reject anything it likes; it may not crash, read out of bounds,
 * or allocate without limit. */
#include "kilix_mux.h"

#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    kmx_grid grid;
    if (kmx_grid_init(&grid, 24, 80) != KMX_OK) return 0;
    /* Return value deliberately ignored: every outcome is acceptable except
     * misbehaviour, which the sanitizers detect. */
    (void)kmx_cells_apply(&grid, data, size);
    kmx_grid_free(&grid);
    return 0;
}
