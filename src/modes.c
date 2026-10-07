#include "kilix_mux_modes.h"

#include <limits.h>
#include <stdlib.h>

#include <vterm.h>

struct kmx_modes {
    VTerm *parser;
    uint32_t flags;
    bool synchronized;
    uint64_t synchronized_generation;
};

static int
discard_text(const char *bytes, size_t len, void *user) {
    size_t consumed = 0;
    (void)user;
    /* Stop before a control so libvterm, rather than the text callback,
     * parses it. UTF-8 continuation bytes are ordinary text here. */
    while (consumed < len && consumed < INT_MAX) {
        unsigned char byte = (unsigned char)bytes[consumed];
        if (byte < 0x20 || byte == 0x7f) break;
        consumed++;
    }
    return (int)consumed;
}

static int
discard_control(unsigned char control, void *user) {
    (void)control;
    (void)user;
    return 1;
}

static void
terminal_reset(kmx_modes *modes) {
    /* Vendored libvterm resets tracking, paste, focus, and cursor on both
     * RIS and DECSTR, but leaves its mouse protocol selection intact. */
    modes->flags &= KMX_MODE_MOUSE_SGR;
    modes->synchronized = false;
}

static int
on_escape(const char *bytes, size_t len, void *user) {
    if (len == 1 && bytes[0] == 'c') terminal_reset(user);
    return 1;
}

static void
set_dec_mode(kmx_modes *modes, long number, bool enabled) {
    uint32_t bit;
    switch (number) {
        case 2026:
            if (enabled && !modes->synchronized) modes->synchronized_generation++;
            modes->synchronized = enabled;
            return;
        case 1: bit = KMX_MODE_APPLICATION_CURSOR; break;
        case 2004: bit = KMX_MODE_BRACKETED_PASTE; break;
        case 1004: bit = KMX_MODE_FOCUS_REPORT; break;
        case 1000:
        case 1002:
        case 1003:
            /* libvterm represents tracking as one mutually exclusive mode.
             * Resetting any tracking mode turns reporting off. */
            modes->flags &= ~KMX_MODE_MOUSE_TRACKING;
            if (!enabled) return;
            bit = number == 1000 ? KMX_MODE_MOUSE_CLICK :
                  number == 1002 ? KMX_MODE_MOUSE_DRAG : KMX_MODE_MOUSE_MOVE;
            break;
        case 1006: bit = KMX_MODE_MOUSE_SGR; break;
        case 1005:
        case 1015:
            /* Unsupported UTF-8/RXVT encodings also replace SGR, even on
             * reset, matching libvterm's protocol selection. */
            modes->flags &= ~KMX_MODE_MOUSE_SGR;
            return;
        default: return;
    }
    if (enabled) modes->flags |= bit;
    else modes->flags &= ~bit;
}

static int
on_csi(const char *leader, const long args[], int argcount,
       const char *intermed, char command, void *user) {
    kmx_modes *modes = user;
    if (!leader && intermed && intermed[0] == '!' && !intermed[1] &&
        command == 'p') {
        terminal_reset(modes);
        return 1;
    }
    if (!leader || leader[0] != '?' || leader[1] || intermed ||
        (command != 'h' && command != 'l')) return 1;
    /* DECSET/DECRST take simple parameters, never colon subparameters. */
    for (int i = 0; i < argcount; i++) {
        if (CSI_ARG_HAS_MORE(args[i])) return 1;
    }
    for (int i = 0; i < argcount; i++) {
        if (!CSI_ARG_IS_MISSING(args[i])) {
            set_dec_mode(modes, CSI_ARG(args[i]), command == 'h');
        }
    }
    return 1;
}

static const VTermParserCallbacks callbacks = {
    .text = discard_text,
    .control = discard_control,
    .escape = on_escape,
    .csi = on_csi,
};

kmx_result
kmx_modes_create(kmx_modes **out) {
    kmx_modes *modes;
    if (!out) return KMX_ERR_INVALID;
    *out = NULL;
    modes = calloc(1, sizeof(*modes));
    if (!modes) return KMX_ERR_MEMORY;
    modes->parser = vterm_new(1, 1);
    if (!modes->parser) {
        free(modes);
        return KMX_ERR_MEMORY;
    }
    vterm_set_utf8(modes->parser, 1);
    vterm_parser_set_callbacks(modes->parser, &callbacks, modes);
    *out = modes;
    return KMX_OK;
}

void
kmx_modes_free(kmx_modes *modes) {
    if (!modes) return;
    vterm_free(modes->parser);
    free(modes);
}

kmx_result
kmx_modes_feed(kmx_modes *modes, const void *data, size_t size) {
    if (!modes || (!data && size)) return KMX_ERR_INVALID;
    if (size && vterm_input_write(modes->parser, data, size) != size) {
        return KMX_ERR_PROTOCOL;
    }
    return KMX_OK;
}

void
kmx_modes_reset(kmx_modes *modes) {
    if (!modes) return;
    /* CAN cancels both an escape in progress and string parser state. */
    vterm_input_write(modes->parser, "\030", 1);
    modes->flags = 0;
    modes->synchronized = false;
}

uint32_t
kmx_modes_get(const kmx_modes *modes) {
    return modes ? modes->flags : 0;
}

bool
kmx_modes_synchronized(const kmx_modes *modes) {
    return modes && modes->synchronized;
}

uint64_t
kmx_modes_synchronized_generation(const kmx_modes *modes) {
    return modes ? modes->synchronized_generation : 0;
}

static bool
flags_valid(uint32_t flags) {
    uint32_t mouse = flags & KMX_MODE_MOUSE_TRACKING;
    return !(flags & ~KMX_MODE_ALL) && !(mouse & (mouse - 1));
}

kmx_result
kmx_modes_encode(uint8_t pane, uint32_t flags,
                 unsigned char out[KMX_MODES_WIRE_SIZE]) {
    if (!out || !flags_valid(flags)) return KMX_ERR_INVALID;
    out[0] = KMX_MODES_WIRE_VERSION;
    out[1] = pane;
    out[2] = 0;
    out[3] = 0;
    out[4] = (unsigned char)(flags >> 24);
    out[5] = (unsigned char)(flags >> 16);
    out[6] = (unsigned char)(flags >> 8);
    out[7] = (unsigned char)flags;
    return KMX_OK;
}

kmx_result
kmx_modes_decode(const void *wire, size_t size, uint8_t *pane, uint32_t *flags) {
    const unsigned char *bytes = wire;
    uint32_t value;
    if (!wire || !pane || !flags) return KMX_ERR_INVALID;
    if (size < KMX_MODES_WIRE_SIZE) return KMX_ERR_TRUNCATED;
    if (size != KMX_MODES_WIRE_SIZE || bytes[0] != KMX_MODES_WIRE_VERSION ||
        bytes[2] || bytes[3]) return KMX_ERR_PROTOCOL;
    value = (uint32_t)bytes[4] << 24 | (uint32_t)bytes[5] << 16 |
            (uint32_t)bytes[6] << 8 | bytes[7];
    if (!flags_valid(value)) return KMX_ERR_PROTOCOL;
    *pane = bytes[1];
    *flags = value;
    return KMX_OK;
}
