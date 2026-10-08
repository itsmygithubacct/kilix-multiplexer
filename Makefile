CC ?= cc
AR ?= ar
PYTHON ?= python3
BUILD_DIR ?= build
PREFIX ?= /usr/local

VTERM_DIR := third_party/libvterm

# One place the version comes from, so a tag and a binary cannot disagree.
VERSION := $(shell cat $(dir $(lastword $(MAKEFILE_LIST)))VERSION 2>/dev/null || echo unknown)

CPPFLAGS += -D_FORTIFY_SOURCE=2 -Iinclude -Isrc -I$(VTERM_DIR)/include
CPPFLAGS += -DKMX_VERSION=\"$(VERSION)\"
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC
LDFLAGS ?=
LDLIBS += -lzstd -lz -lm

# Optional installed EnCodec consumer. The supplied native library must be
# built with ONNX=1 CONTENT=1 and an exact reviewed CONTENT_SOURCE/COMMIT.
ENCODEC ?= 0
ENCODEC_CFLAGS ?= $(shell pkg-config --cflags kilix-encodec 2>/dev/null)
ENCODEC_LIBS ?= $(shell pkg-config --libs kilix-encodec 2>/dev/null)
ifeq ($(ENCODEC),1)
CPPFLAGS += -DKMX_HAVE_ENCODEC=1 $(ENCODEC_CFLAGS)
CODEC_LIBS := $(ENCODEC_LIBS) -lsamplerate -pthread
endif

# Vendored with recorded patches; built with upstream's own warning posture
# rather than ours, so our -Werror never depends on someone else's code.
VTERM_CFLAGS := -O2 -std=c99 -fPIC -w

LIB_SOURCES := src/grid.c src/codec.c src/term.c src/modes.c src/input.c src/input_session.c src/sync.c src/frame.c src/render.c src/predict.c src/layout.c src/graphics.c src/motion.c src/audio.c src/endpoint.c
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(BUILD_DIR)/%.o)
VTERM_SOURCES := $(wildcard $(VTERM_DIR)/src/*.c)
VTERM_OBJECTS := $(VTERM_SOURCES:%.c=$(BUILD_DIR)/%.o)

STATIC_LIB := $(BUILD_DIR)/libkilix-mux.a
SHARED_LIB := $(BUILD_DIR)/libkilix-mux.so
TEST := $(BUILD_DIR)/test-mux
MODES_TEST := $(BUILD_DIR)/test-modes
INPUT_WIRE_TEST := $(BUILD_DIR)/test-input
INPUT_SESSION_TEST := $(BUILD_DIR)/test-input-session
INPUT_JOURNAL_TEST := $(BUILD_DIR)/test-input-journal
INFLIGHT_TEST := $(BUILD_DIR)/test-sync-inflight
TAP_TEST := $(BUILD_DIR)/test-tap
FUZZ := $(BUILD_DIR)/fuzz-decoders
BENCH := $(BUILD_DIR)/kmx-bench
SERVE := $(BUILD_DIR)/kmx-serve
SHAPE := $(BUILD_DIR)/kmx-shape
ATTACH := $(BUILD_DIR)/kmx-attach
TLS_WAIT_ATTACH := $(BUILD_DIR)/kmx-attach-tls-write-wait
FLOOD := $(BUILD_DIR)/flood-input
INPUT_TEST := $(BUILD_DIR)/test-input-transform
ENCODEC_TEST := $(BUILD_DIR)/test-encodec

.PHONY: all clean test test-input test-coding sanitize fuzz backpressure churn check-vendor install

all: $(STATIC_LIB) $(BENCH) $(SERVE) $(ATTACH) $(SHAPE)

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/src/%.o: src/%.c include/kilix_mux.h include/kilix_mux_modes.h include/kilix_mux_input.h include/kilix_mux_input_session.h | $(BUILD_DIR)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/$(VTERM_DIR)/src/%.o: $(VTERM_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p "$(dir $@)"
	$(CC) -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src $(VTERM_CFLAGS) -c "$<" -o "$@"

$(STATIC_LIB): $(LIB_OBJECTS) $(VTERM_OBJECTS)
	$(AR) rcs "$@" $(LIB_OBJECTS) $(VTERM_OBJECTS)

# The Python coding-session harness uses the native decoder through ctypes.
$(SHARED_LIB): $(STATIC_LIB)
	$(CC) $(LDFLAGS) -shared -o "$@" -Wl,--whole-archive $(STATIC_LIB) -Wl,--no-whole-archive $(LDLIBS)

$(BUILD_DIR)/kmx_bench.o: tools/kmx_bench.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BENCH): $(BUILD_DIR)/kmx_bench.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_bench.o $(STATIC_LIB) $(LDLIBS) -lutil

$(BUILD_DIR)/kmx_serve.o: tools/kmx_serve.c tools/kmx_pixel.h tools/kmx_tap.h tools/kmx_encodec.h tools/kmx_random.h include/kilix_mux.h include/kilix_mux_modes.h include/kilix_mux_input.h include/kilix_mux_input_session.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(BUILD_DIR)/kmx_pixel.o: tools/kmx_pixel.c tools/kmx_pixel.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(BUILD_DIR)/kmx_tap.o: tools/kmx_tap.c tools/kmx_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(BUILD_DIR)/kmx_shape.o: tools/kmx_shape.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(SHAPE): $(BUILD_DIR)/kmx_shape.o
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_shape.o

$(BUILD_DIR)/kmx_tls.o: tools/kmx_tls.c tools/kmx_tls.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(BUILD_DIR)/kmx_encodec.o: tools/kmx_encodec.c tools/kmx_encodec.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(SERVE): $(BUILD_DIR)/kmx_serve.o $(BUILD_DIR)/kmx_pixel.o $(BUILD_DIR)/kmx_tap.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_serve.o $(BUILD_DIR)/kmx_pixel.o $(BUILD_DIR)/kmx_tap.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB) $(LDLIBS) $(CODEC_LIBS) -lutil -lssl -lcrypto

$(BUILD_DIR)/kmx_attach.o: tools/kmx_attach.c tools/kmx_tls.h tools/kmx_input_transform.h tools/kmx_encodec.h tools/kmx_read_clock.h tools/kmx_random.h include/kilix_mux.h include/kilix_mux_modes.h include/kilix_mux_input.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(BUILD_DIR)/kmx_input_transform.o: tools/kmx_input_transform.c tools/kmx_input_transform.h include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(ATTACH): $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB) $(LDLIBS) $(CODEC_LIBS) -lssl -lcrypto

$(TLS_WAIT_ATTACH): tests/tls_write_wait_shim.c $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/tls_write_wait_shim.c $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB) -Wl,--wrap=SSL_write -Wl,--wrap=SSL_get_error $(LDLIBS) $(CODEC_LIBS) -lssl -lcrypto

$(BUILD_DIR)/test_mux.o: tests/test_mux.c include/kilix_mux.h tools/kmx_read_clock.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(TEST): $(BUILD_DIR)/test_mux.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_mux.o $(STATIC_LIB) $(LDLIBS)

$(MODES_TEST): tests/test_modes.c include/kilix_mux_modes.h $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_modes.c $(STATIC_LIB) $(LDLIBS)

$(INFLIGHT_TEST): tests/test_sync_inflight.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_sync_inflight.c $(STATIC_LIB) $(LDLIBS)

$(BUILD_DIR)/test_tap.o: tests/test_tap.c tools/kmx_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(TAP_TEST): $(BUILD_DIR)/test_tap.o $(BUILD_DIR)/kmx_tap.o
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_tap.o $(BUILD_DIR)/kmx_tap.o

$(BUILD_DIR)/test_input_transform.o: tests/test_input_transform.c tools/kmx_input_transform.h include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(INPUT_TEST): $(BUILD_DIR)/test_input_transform.o $(BUILD_DIR)/kmx_input_transform.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_input_transform.o $(BUILD_DIR)/kmx_input_transform.o $(STATIC_LIB) $(LDLIBS)

$(BUILD_DIR)/flood_input.o: tests/flood_input.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(FLOOD): $(BUILD_DIR)/flood_input.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/flood_input.o $(STATIC_LIB) $(LDLIBS)

# A pane that stops reading its input must not stop the server.  Separate from
# `test` because it starts a real server and a real pty rather than exercising
# the library.
backpressure: $(SERVE) $(FLOOD)
	tests/backpressure.sh

# Abandon connections at every stage of the handshake, under ASan.  Builds its
# own sanitized server and leaves the tree clean afterwards, so it is not part
# of `all`.
churn:
	tests/churn.sh

$(ENCODEC_TEST): tests/test_encodec.c tools/kmx_read_clock.h $(BUILD_DIR)/kmx_encodec.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools $(LDFLAGS) -o "$@" tests/test_encodec.c $(BUILD_DIR)/kmx_encodec.o $(CODEC_LIBS) -lm

# Explicit local graph input is required to run this scheduling regression.
$(BUILD_DIR)/test-encodec-age: tests/test_encodec_age.c $(BUILD_DIR)/kmx_encodec.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools $(LDFLAGS) -Wl,--wrap=kenc_encoder_push_s16 -o "$@" tests/test_encodec_age.c $(BUILD_DIR)/kmx_encodec.o $(CODEC_LIBS) -lm

$(INPUT_WIRE_TEST): tests/test_input.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_input.c $(STATIC_LIB) $(LDLIBS)

$(INPUT_SESSION_TEST): tests/test_input_session.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_input_session.c $(STATIC_LIB) $(LDLIBS)

$(INPUT_JOURNAL_TEST): tests/test_input_journal.c tools/kmx_attach.c tools/kmx_random.h include/kilix_mux_input.h $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools $(LDFLAGS) -o "$@" tests/test_input_journal.c $(BUILD_DIR)/kmx_input_transform.o $(BUILD_DIR)/kmx_tls.o $(BUILD_DIR)/kmx_encodec.o $(STATIC_LIB) -Wl,--wrap=send -Wl,--wrap=kmx_tls_write -Wl,--wrap=kmx_tls_write_wants_read $(LDLIBS) $(CODEC_LIBS) -lssl -lcrypto

# Wire, ownership and real client journal tests need no listening socket.
test-input: $(INPUT_WIRE_TEST) $(INPUT_SESSION_TEST) $(INPUT_JOURNAL_TEST)
	$(TEST_ENVIRONMENT) "$(INPUT_WIRE_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_SESSION_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_JOURNAL_TEST)"

test: $(TEST) $(MODES_TEST) $(INPUT_WIRE_TEST) $(INPUT_SESSION_TEST) $(INPUT_JOURNAL_TEST) $(INFLIGHT_TEST) $(TAP_TEST) $(INPUT_TEST) $(ENCODEC_TEST)
	$(TEST_ENVIRONMENT) "$(TEST)"
	$(TEST_ENVIRONMENT) "$(MODES_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_WIRE_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_SESSION_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_JOURNAL_TEST)"
	$(TEST_ENVIRONMENT) "$(INFLIGHT_TEST)"
	$(TEST_ENVIRONMENT) "$(TAP_TEST)"
	$(TEST_ENVIRONMENT) "$(INPUT_TEST)"
	$(TEST_ENVIRONMENT) "$(ENCODEC_TEST)"
	$(PYTHON) tests/test_remote_chrome.py
	$(PYTHON) tests/test_pixel_input.py

# Starts owned PTYs and Unix sockets; kept separate for restricted sandboxes.
test-coding: $(SERVE) $(ATTACH) $(TLS_WAIT_ATTACH) $(SHARED_LIB)
	$(PYTHON) tests/test_coding_terminal.py --server "$(SERVE)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_attach_modes.py --server "$(SERVE)" --attach "$(ATTACH)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_attach_backpressure.py --attach "$(ATTACH)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_synchronized_output.py --server "$(SERVE)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_tls_write_wait.py --attach "$(TLS_WAIT_ATTACH)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_input_resume.py --server "$(SERVE)" --attach "$(ATTACH)" --library "$(SHARED_LIB)"
	$(PYTHON) tests/test_input_tls_resume.py --server "$(SERVE)" --attach "$(ATTACH)" --library "$(SHARED_LIB)"

TEST_ENVIRONMENT ?=

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -fsanitize=address,undefined" \
		VTERM_CFLAGS="-O1 -g -std=c99 -fPIC -w -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined" \
		TEST_ENVIRONMENT="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" \
		test
	@# Leave the tree consistent.  Sanitized objects linked into an ordinary
	@# build fail with undefined __asan_* symbols, which reads as a mysterious
	@# link error rather than as leftovers from this target.
	$(MAKE) clean

SEED := $(BUILD_DIR)/seed-corpus
CORPUS := $(BUILD_DIR)/corpus

$(BUILD_DIR)/seed_corpus.o: tests/seed_corpus.c include/kilix_mux.h include/kilix_mux_modes.h include/kilix_mux_input.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(SEED): $(BUILD_DIR)/seed_corpus.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/seed_corpus.o $(STATIC_LIB) $(LDLIBS)

# Every decoder a remote peer can reach is fuzzed, not merely tested.
# Requires a clang with libFuzzer.
#
# Seeded with real messages, each also truncated and bit-flipped.  Worth about
# 120 coverage points at a ten-second budget and almost nothing at sixty, so it
# earns its place here and in CI rather than in a long soak.
fuzz: $(BUILD_DIR) $(SEED)
	"$(SEED)" "$(CORPUS)"
	clang -std=c11 $(CPPFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined \
		-o "$(FUZZ)" tests/fuzz_decoders.c $(LIB_SOURCES) $(VTERM_SOURCES) $(LDLIBS)
	"$(FUZZ)" "$(CORPUS)" -max_total_time=$${FUZZ_SECONDS:-30} -print_final_stats=1

# Reverse recorded patches in a private copy, then verify original upstream hashes.
check-vendor:
	$(PYTHON) tools/check_vendor.py

clean:
	rm -rf -- "$(BUILD_DIR)"

# The library was the only thing installed for a while, which made `make
# install` produce a header and an archive and none of the two programs the
# project exists to provide.
install: all
	install -d "$(DESTDIR)$(PREFIX)/bin" \
	          "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib"
	install -m 0755 "$(SERVE)" "$(DESTDIR)$(PREFIX)/bin/"
	install -m 0755 "$(ATTACH)" "$(DESTDIR)$(PREFIX)/bin/"
	install -m 0755 "$(SHAPE)" "$(DESTDIR)$(PREFIX)/bin/"
	install -m 0644 include/kilix_mux.h include/kilix_mux_modes.h include/kilix_mux_input.h include/kilix_mux_input_session.h "$(DESTDIR)$(PREFIX)/include/"
	install -m 0644 "$(STATIC_LIB)" "$(DESTDIR)$(PREFIX)/lib/"
