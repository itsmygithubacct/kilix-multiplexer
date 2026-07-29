#ifndef KMX_TAP_H
#define KMX_TAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KMX_TAP_SESSION_MAX 64

typedef struct {
    int listener;
    int source;
    char path[108];
    char session[KMX_TAP_SESSION_MAX + 1];
    unsigned char header[112];
    size_t header_used;
    unsigned char *frame;
    size_t frame_capacity;
    size_t frame_size;
    size_t frame_used;
    int width;
    int height;
    int columns;
    int rows;
    int scroll_x;
    int scroll_y;
    uint64_t offered_micros;
    bool have_frame;
    bool owns_path;
} kmx_tap;

bool kmx_tap_start(kmx_tap *tap, const char *path, const char *session);
void kmx_tap_stop(kmx_tap *tap);

int kmx_tap_listener_fd(const kmx_tap *tap);
int kmx_tap_source_fd(const kmx_tap *tap);

/*
 * Consume readiness from the listener and producer descriptors.  A complete
 * newest frame, if one arrived, is retained until kmx_tap_take() is called.
 */
void kmx_tap_poll(kmx_tap *tap, short listener_events, short source_events);

bool kmx_tap_take(
    kmx_tap *tap,
    const unsigned char **rgb,
    int *width,
    int *height,
    int *columns,
    int *rows,
    int *scroll_x,
    int *scroll_y,
    uint64_t *offered_micros
);

#endif
