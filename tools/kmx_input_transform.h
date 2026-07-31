#ifndef KILIX_MUX_INPUT_TRANSFORM_H
#define KILIX_MUX_INPUT_TRANSFORM_H

#include "kilix_mux.h"

#include <stdbool.h>
#include <stddef.h>

/* Scale SGR-pixel mouse reports while preserving every other input sequence.
 * `pending` retains a CSI split across reads. Enhanced Ctrl-] sets `detach`
 * and is consumed instead of crossing the network. */
int kmx_pixel_input_transform(
    kmx_buffer *pending,
    const void *data,
    size_t size,
    int local_width,
    int local_height,
    int remote_width,
    int remote_height,
    kmx_buffer *out,
    bool *detach);

#endif
