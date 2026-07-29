#ifndef KILIX_MUX_PIXEL_H
#define KILIX_MUX_PIXEL_H

/* A pane whose content is pixels: an X client on a private display, captured
 * frame by frame.  This is what the motion plane exists for, and it is also
 * how that plane gets tested against something real rather than a generator.
 *
 * The display is private to the session, so a pixel pane can never draw on
 * whatever screen the operator is actually using. */

#include <stdbool.h>
#include <sys/types.h>

typedef struct {
    int display;        /* the private X display number */
    pid_t server;       /* Xvfb */
    pid_t app;          /* the X client */
    pid_t capture;      /* the frame source */
    int frames;         /* read end of the raw frame stream */
    int width;
    int height;
    unsigned char *frame;
    size_t frame_bytes;
    size_t filled;
} kmx_pixel_pane;

/* Start a private display, run `command` on it, and begin capturing.
 * `fps` bounds the capture rate. */
bool kmx_pixel_start(
    kmx_pixel_pane *pane,
    const char *command,
    int width,
    int height,
    int fps
);

/* Read whatever is available.  Returns true when a whole frame is ready in
 * pane->frame; call again until it returns false. */
bool kmx_pixel_poll(kmx_pixel_pane *pane, bool *closed);

void kmx_pixel_stop(kmx_pixel_pane *pane);

#endif
