#include "kilix_mux_modes.h"

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
feed(kmx_modes *modes, const char *text) {
    require(kmx_modes_feed(modes, text, strlen(text)) == KMX_OK, "feed succeeds");
}

static void
expect(kmx_modes *modes, uint32_t flags, const char *message) {
    require(kmx_modes_get(modes) == flags, message);
}

static void
test_fragmentation(kmx_modes *modes) {
    const char *text = "ordinary \342\230\203\033[?1;2004;1004;1002;1006h"
                       "\033[?1;1004l\033[?1003h\033[2J";
    size_t len = strlen(text);
    uint32_t wanted = KMX_MODE_BRACKETED_PASTE | KMX_MODE_MOUSE_MOVE |
                      KMX_MODE_MOUSE_SGR;
    for (size_t split = 0; split <= len; split++) {
        kmx_modes_reset(modes);
        require(kmx_modes_feed(modes, text, split) == KMX_OK, "feed first fragment");
        require(kmx_modes_feed(modes, text + split, len - split) == KMX_OK,
                "feed second fragment");
        expect(modes, wanted, "every two-fragment split has the same modes");
    }
    kmx_modes_reset(modes);
    for (size_t i = 0; i < len; i++) {
        require(kmx_modes_feed(modes, text + i, 1) == KMX_OK, "feed byte fragment");
    }
    expect(modes, wanted, "one-byte fragments preserve escape state");
    kmx_modes_reset(modes);
    feed(modes, "\033[?2004");
    expect(modes, 0, "unfinished mode sequence does not take effect");
    feed(modes, "h");
    expect(modes, KMX_MODE_BRACKETED_PASTE, "mode takes effect on final byte");
}

static void
test_strings(kmx_modes *modes) {
    const char *strings[] = {
        "\033]0;title [?2004l\007",
        "\033]0;title \033[?2004l\007",
        "\033_Gpayload [?2004l\033\\",
        "\033_Gpayload \033[?2004l\033\\",
        "\033Pqpayload [?2004l\033\\",
        "\033^payload [?2004l\033\\",
        "\033Xpayload [?2004l\033\\",
        "\033]0;utf8 \302\233?2004l\007",
        "\033_Gutf8 \302\233?2004l\033\\",
    };
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        size_t len = strlen(strings[i]);
        for (size_t split = 0; split <= len; split++) {
            kmx_modes_reset(modes);
            feed(modes, "\033[?2004h");
            require(kmx_modes_feed(modes, strings[i], split) == KMX_OK,
                    "feed first string fragment");
            require(kmx_modes_feed(modes, strings[i] + split, len - split) == KMX_OK,
                    "feed second string fragment");
            expect(modes, KMX_MODE_BRACKETED_PASTE,
                   "CSI-looking string data does not change modes");
            feed(modes, "\033[?2004l");
            expect(modes, 0, "mode sequence after string is parsed normally");
        }
    }
    kmx_modes_reset(modes);
    feed(modes, "\033]0;unterminated");
    feed(modes, "[?2004h");
    expect(modes, 0, "unterminated string payload does not set mode");
    feed(modes, "\007\033[?2004h");
    expect(modes, KMX_MODE_BRACKETED_PASTE, "OSC terminator resumes mode parsing");
}

static void
test_mode_semantics(kmx_modes *modes) {
    const uint32_t ordinary = KMX_MODE_APPLICATION_CURSOR |
                              KMX_MODE_BRACKETED_PASTE | KMX_MODE_FOCUS_REPORT;
    kmx_modes_reset(modes);
    feed(modes, "\033[?1;2004;1004h");
    expect(modes, ordinary, "multiple DECSET parameters enable modes");
    feed(modes, "\033[?1000h");
    expect(modes, ordinary | KMX_MODE_MOUSE_CLICK, "click tracking enables");
    feed(modes, "\033[?1002h");
    expect(modes, ordinary | KMX_MODE_MOUSE_DRAG, "drag replaces click");
    feed(modes, "\033[?1003h");
    expect(modes, ordinary | KMX_MODE_MOUSE_MOVE, "move replaces drag");
    feed(modes, "\033[?1000l");
    expect(modes, ordinary, "resetting any tracking mode disables reporting");
    feed(modes, "\033[?1000;1002;1003;1006h");
    expect(modes, ordinary | KMX_MODE_MOUSE_MOVE | KMX_MODE_MOUSE_SGR,
           "multiple mouse parameters apply in order");
    feed(modes, "\033[?1006l");
    expect(modes, ordinary | KMX_MODE_MOUSE_MOVE, "SGR reset preserves tracking");
    feed(modes, "\033[?1006h\033[?1005h");
    expect(modes, ordinary | KMX_MODE_MOUSE_MOVE, "UTF8 protocol replaces SGR");
    feed(modes, "\033[?1006h\033[?1015l");
    expect(modes, ordinary | KMX_MODE_MOUSE_MOVE, "RXVT reset also replaces SGR");
    feed(modes, "\033[?1;2004;1004;1003l");
    expect(modes, 0, "multiple DECRST parameters disable modes");
    feed(modes, "\033[?;2004;;h");
    expect(modes, KMX_MODE_BRACKETED_PASTE, "missing arguments are ignored");
    feed(modes, "\033[?2004;2004l");
    expect(modes, 0, "repeated arguments are harmless");
}

static void
test_resets(kmx_modes *modes) {
    for (int soft = 0; soft < 2; soft++) {
        kmx_modes_reset(modes);
        feed(modes, "\033[?1;2004;1004;1003;1006h");
        feed(modes, soft ? "\033[!p" : "\033c");
        expect(modes, KMX_MODE_MOUSE_SGR,
               "RIS and DECSTR clear modes while preserving mouse protocol");
        feed(modes, "\033[?1000h");
        expect(modes, KMX_MODE_MOUSE_CLICK | KMX_MODE_MOUSE_SGR,
               "mouse protocol survives reset and new tracking");
    }
    feed(modes, "\033[?2004");
    kmx_modes_reset(modes);
    feed(modes, "h");
    expect(modes, 0, "explicit reset clears flags and pending CSI");
    feed(modes, "\033_Gunterminated");
    kmx_modes_reset(modes);
    feed(modes, "\033[?2004h");
    expect(modes, KMX_MODE_BRACKETED_PASTE, "explicit reset discards pending APC");
    feed(modes, "\033");
    kmx_modes_reset(modes);
    feed(modes, "c\033[?1h");
    expect(modes, KMX_MODE_APPLICATION_CURSOR, "reset discards a pending ESC");
}

static void
test_unknown_and_invalid(kmx_modes *modes) {
    const char *ignored[] = {
        "plain text [?2004l", "\033[2004l", "\033[>2004l", "\033[??2004l",
        "\033[?2004$l", "\033[?2004m", "\033[?9999;0h", "\033[?h",
        "\033[?2004:0l", "\033[?2004:1;1l", "\033[>1u", "\033=", "\033>",
        "\033[?2004\030l", "\033[?2004\032l", "\033[?!p", "\033 !p",
    };
    kmx_modes_reset(modes);
    feed(modes, "\033[?2004h");
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        feed(modes, ignored[i]);
        expect(modes, KMX_MODE_BRACKETED_PASTE,
               "unknown, cancelled and invalid commands preserve tracked state");
    }
    require(kmx_modes_feed(modes, NULL, 0) == KMX_OK, "empty feed is valid");
    require(kmx_modes_feed(modes, NULL, 1) == KMX_ERR_INVALID,
            "nonempty null feed is invalid");
    require(kmx_modes_feed(NULL, "x", 1) == KMX_ERR_INVALID,
            "null tracker feed is invalid");
    require(kmx_modes_create(NULL) == KMX_ERR_INVALID, "null creation is invalid");
    require(kmx_modes_get(NULL) == 0, "null tracker has no modes");
    kmx_modes_free(NULL);
    kmx_modes_reset(NULL);
}

static void
expect_decode_error(const unsigned char *wire, size_t size, kmx_result result) {
    uint8_t pane = 201;
    uint32_t flags = UINT32_C(0xdeadbeef);
    require(kmx_modes_decode(wire, size, &pane, &flags) == result,
            "malformed payload is rejected");
    require(pane == 201 && flags == UINT32_C(0xdeadbeef),
            "decode failure preserves destination");
}

static void
test_wire(void) {
    unsigned char wire[KMX_MODES_WIRE_SIZE + 1];
    uint8_t pane;
    uint32_t flags;
    for (uint32_t value = 0; value <= KMX_MODE_ALL; value++) {
        uint32_t mouse = value & KMX_MODE_MOUSE_TRACKING;
        bool valid = !(mouse & (mouse - 1));
        memset(wire, 0xa5, sizeof(wire));
        require(kmx_modes_encode(255, value, wire) ==
                (valid ? KMX_OK : KMX_ERR_INVALID), "all seven-bit flag combinations");
        if (!valid) {
            for (size_t i = 0; i < sizeof(wire); i++) {
                require(wire[i] == 0xa5, "invalid encode leaves destination intact");
            }
            continue;
        }
        require(wire[0] == 1 && wire[1] == 255 && !wire[2] && !wire[3] &&
                !wire[4] && !wire[5] && !wire[6] && wire[7] == value,
                "wire bytes match version, pane, reserved and network-order flags");
        require(wire[KMX_MODES_WIRE_SIZE] == 0xa5, "encode writes exactly eight bytes");
        require(kmx_modes_decode(wire, KMX_MODES_WIRE_SIZE, &pane, &flags) == KMX_OK,
                "valid payload decodes");
        require(pane == 255 && flags == value, "codec round trip");
    }
    require(kmx_modes_encode(0, 0, wire) == KMX_OK, "zero flags encode");
    for (size_t size = 0; size < KMX_MODES_WIRE_SIZE; size++) {
        expect_decode_error(wire, size, KMX_ERR_TRUNCATED);
    }
    expect_decode_error(wire, KMX_MODES_WIRE_SIZE + 1, KMX_ERR_PROTOCOL);
    for (unsigned int byte = 0; byte <= 255; byte++) {
        if (byte != KMX_MODES_WIRE_VERSION) {
            wire[0] = (unsigned char)byte;
            expect_decode_error(wire, KMX_MODES_WIRE_SIZE, KMX_ERR_PROTOCOL);
        }
        wire[0] = KMX_MODES_WIRE_VERSION;
        if (!byte) continue;
        for (size_t reserved = 2; reserved <= 3; reserved++) {
            wire[reserved] = (unsigned char)byte;
            expect_decode_error(wire, KMX_MODES_WIRE_SIZE, KMX_ERR_PROTOCOL);
            wire[reserved] = 0;
        }
    }
    for (unsigned int bit = 7; bit < 32; bit++) {
        uint32_t bad = UINT32_C(1) << bit;
        require(kmx_modes_encode(0, bad, wire) == KMX_ERR_INVALID,
                "unknown flags cannot encode");
        wire[4] = (unsigned char)(bad >> 24);
        wire[5] = (unsigned char)(bad >> 16);
        wire[6] = (unsigned char)(bad >> 8);
        wire[7] = (unsigned char)bad;
        expect_decode_error(wire, KMX_MODES_WIRE_SIZE, KMX_ERR_PROTOCOL);
    }
    require(kmx_modes_encode(0, 0, wire) == KMX_OK, "clear malformed bytes");
    wire[7] = KMX_MODE_MOUSE_CLICK | KMX_MODE_MOUSE_DRAG;
    expect_decode_error(wire, KMX_MODES_WIRE_SIZE, KMX_ERR_PROTOCOL);
    require(kmx_modes_encode(0, 0, NULL) == KMX_ERR_INVALID, "null wire output rejected");
    require(kmx_modes_decode(NULL, 0, &pane, &flags) == KMX_ERR_INVALID,
            "null wire input rejected");
    require(kmx_modes_decode(wire, sizeof(wire), NULL, &flags) == KMX_ERR_INVALID,
            "null pane output rejected");
    require(kmx_modes_decode(wire, sizeof(wire), &pane, NULL) == KMX_ERR_INVALID,
            "null flags output rejected");
}

static void
test_synchronized_output(kmx_modes *modes) {
    const char *begin = "\033[?2026h";
    size_t size = strlen(begin);
    uint64_t generation;
    for (size_t split = 0; split < size; split++) {
        kmx_modes_reset(modes);
        require(!kmx_modes_synchronized(modes), "synchronized output starts inactive");
        require(kmx_modes_feed(modes, begin, split) == KMX_OK, "partial synchronized begin");
        require(!kmx_modes_synchronized(modes), "incomplete begin does not hold output");
        require(kmx_modes_feed(modes, begin + split, size - split) == KMX_OK,
                "finish synchronized begin");
        require(kmx_modes_synchronized(modes), "fragmented begin holds output");
        generation = kmx_modes_synchronized_generation(modes);
        feed(modes, "\033[?2026hordinary output\033[?1;2004h");
        require(kmx_modes_synchronized_generation(modes) == generation,
                "repeated set does not restart synchronized output");
        expect(modes, KMX_MODE_APPLICATION_CURSOR | KMX_MODE_BRACKETED_PASTE,
               "synchronized state is independent of input flags");
        feed(modes, "\033[?2026l\033[?2026h");
        require(kmx_modes_synchronized(modes) &&
                kmx_modes_synchronized_generation(modes) == generation + 1,
                "end and new begin within one feed start another block");
        feed(modes, "\033[?2026");
        require(kmx_modes_synchronized(modes), "incomplete end retains hold");
        feed(modes, "l");
        require(!kmx_modes_synchronized(modes), "fragmented end releases hold");
    }
    kmx_modes_reset(modes);
    feed(modes, "\033]0;fake [?2026h\007\033_Gfake [?2026h\033\\");
    require(!kmx_modes_synchronized(modes), "string-contained fake begin is ignored");
    feed(modes, "\033[?2026:0h\033[2026h\033[>2026h");
    require(!kmx_modes_synchronized(modes), "invalid synchronized selectors ignored");
    feed(modes, "\033[?2026h\033c");
    require(!kmx_modes_synchronized(modes), "RIS releases synchronized hold");
    feed(modes, "\033[?2026h\033[!p");
    require(!kmx_modes_synchronized(modes), "soft reset releases synchronized hold");
    feed(modes, "\033[?2026h");
    kmx_modes_reset(modes);
    require(!kmx_modes_synchronized(modes) && !kmx_modes_synchronized(NULL),
            "explicit and null reset state are inactive");
}

int
main(void) {
    kmx_modes *modes;
    require(kmx_modes_create(&modes) == KMX_OK, "create mode observer");
    expect(modes, 0, "initial modes are clear");
    test_fragmentation(modes);
    test_strings(modes);
    test_mode_semantics(modes);
    test_resets(modes);
    test_unknown_and_invalid(modes);
    test_wire();
    test_synchronized_output(modes);
    kmx_modes_free(modes);
    puts("all terminal mode checks passed");
    return 0;
}
