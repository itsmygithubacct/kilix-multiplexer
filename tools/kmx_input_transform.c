#include "kmx_input_transform.h"

#include <stdio.h>
#include <string.h>

static int
scaled_coordinate(int value, int local_extent, int remote_extent) {
    long long numerator;
    if (local_extent <= 1 || remote_extent <= 1) return 0;
    if (value < 0) value = 0;
    if (value >= local_extent) value = local_extent - 1;
    numerator =
        (long long)value * (remote_extent - 1) + (local_extent - 1) / 2;
    return (int)(numerator / (local_extent - 1));
}

int
kmx_pixel_input_transform(
    kmx_buffer *pending,
    const void *data,
    size_t size,
    int local_width,
    int local_height,
    int remote_width,
    int remote_height,
    kmx_buffer *out,
    bool *detach
) {
    size_t at = 0;
    if (!pending || (!data && size) || !out || !detach) return -1;
    if (kmx_buffer_append(pending, data, size) != KMX_OK) return -1;
    kmx_buffer_reset(out);
    while (at < pending->size) {
        size_t end;
        size_t length;
        unsigned char final;
        char sequence[128];

        if (pending->data[at] != '\033' ||
            at + 1 >= pending->size ||
            pending->data[at + 1] != '[') {
            if (pending->data[at] == '\033' && at + 1 == pending->size) break;
            if (kmx_buffer_append(out, pending->data + at, 1) != KMX_OK) {
                return -1;
            }
            at++;
            continue;
        }

        end = at + 2;
        while (end < pending->size &&
               !(pending->data[end] >= 0x40 && pending->data[end] <= 0x7e)) {
            end++;
        }
        if (end == pending->size) {
            if (pending->size - at < sizeof sequence) break;
            if (kmx_buffer_append(out, pending->data + at, 1) != KMX_OK) {
                return -1;
            }
            at++;
            continue;
        }

        length = end - at + 1;
        final = pending->data[end];
        if (length < sizeof sequence) {
            int code = 0;
            int modifiers = 1;
            memcpy(sequence, pending->data + at, length);
            sequence[length] = '\0';

            if (final == 'u' &&
                sscanf(sequence, "\033[%d;%d", &code, &modifiers) == 2 &&
                code == ']' && ((modifiers - 1) & 4)) {
                *detach = true;
                at = end + 1;
                continue;
            }

            if ((final == 'M' || final == 'm') &&
                length >= 4 && sequence[2] == '<') {
                unsigned button = 0;
                int x = 0;
                int y = 0;
                char parsed_final = '\0';
                int consumed = 0;
                if (sscanf(
                        sequence, "\033[<%u;%d;%d%c%n",
                        &button, &x, &y, &parsed_final, &consumed) == 4 &&
                    consumed == (int)length &&
                    (parsed_final == 'M' || parsed_final == 'm') &&
                    local_width > 0 && local_height > 0 &&
                    remote_width > 0 && remote_height > 0) {
                    char rewritten[128];
                    int rewritten_size;
                    x = scaled_coordinate(x, local_width, remote_width);
                    y = scaled_coordinate(y, local_height, remote_height);
                    rewritten_size = snprintf(
                        rewritten, sizeof rewritten, "\033[<%u;%d;%d%c",
                        button, x, y, parsed_final);
                    if (rewritten_size < 0 ||
                        (size_t)rewritten_size >= sizeof rewritten ||
                        kmx_buffer_append(
                            out, rewritten, (size_t)rewritten_size) != KMX_OK) {
                        return -1;
                    }
                    at = end + 1;
                    continue;
                }
            }
        }
        if (kmx_buffer_append(out, pending->data + at, length) != KMX_OK) {
            return -1;
        }
        at = end + 1;
    }
    if (at) {
        memmove(pending->data, pending->data + at, pending->size - at);
        pending->size -= at;
    }
    return 0;
}
