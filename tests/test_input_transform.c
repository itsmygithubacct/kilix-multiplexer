#include "kilix_mux.h"
#include "kmx_input_transform.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
require(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL  %s\n", message);
    exit(1);
}

static void
expect_bytes(const kmx_buffer *buffer, const char *expected, const char *message) {
    size_t size = strlen(expected);
    require(buffer->size == size, message);
    require(memcmp(buffer->data, expected, size) == 0, message);
}

int
main(void) {
    kmx_buffer pending;
    kmx_buffer out;
    bool detach = false;
    const char *input;

    kmx_buffer_init(&pending);
    kmx_buffer_init(&out);

    input = "plain";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "plain input transforms");
    expect_bytes(&out, "plain", "plain input is unchanged");
    require(!detach && pending.size == 0, "plain input leaves no state");

    input = "\033[<0;99";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "a split mouse report starts");
    require(out.size == 0 && pending.size == strlen(input),
            "a split report is retained");
    input = ";49M";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "a split mouse report completes");
    expect_bytes(&out, "\033[<0;199;99M", "mouse coordinates scale to the frame");
    require(pending.size == 0, "completed mouse report leaves no state");

    input = "\033[<0;-5;80m";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "out-of-bounds mouse input transforms");
    expect_bytes(&out, "\033[<0;0;99m", "mouse coordinates are clamped");

    detach = false;
    input = "before\033[93;5uafter";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "enhanced detach input transforms");
    expect_bytes(&out, "beforeafter", "enhanced Ctrl-] is consumed locally");
    require(detach, "enhanced Ctrl-] requests detach");

    detach = false;
    input = "\033[65;1u";
    require(
        kmx_pixel_input_transform(
            &pending, input, strlen(input),
            100, 50, 200, 100, &out, &detach) == 0,
        "ordinary CSI-u input transforms");
    expect_bytes(&out, "\033[65;1u", "ordinary CSI-u input is unchanged");
    require(!detach, "ordinary CSI-u does not detach");

    kmx_buffer_free(&out);
    kmx_buffer_free(&pending);
    puts("all pixel input transformation checks passed");
    return 0;
}
