CC ?= cc
AR ?= ar
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

# Vendored verbatim; built with the upstream project's own warning posture
# rather than ours, so our -Werror never depends on someone else's code.
VTERM_CFLAGS := -O2 -std=c99 -fPIC -w

LIB_SOURCES := src/grid.c src/codec.c src/term.c src/sync.c src/frame.c src/render.c src/predict.c src/layout.c src/graphics.c src/motion.c src/audio.c src/endpoint.c
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(BUILD_DIR)/%.o)
VTERM_SOURCES := $(wildcard $(VTERM_DIR)/src/*.c)
VTERM_OBJECTS := $(VTERM_SOURCES:%.c=$(BUILD_DIR)/%.o)

STATIC_LIB := $(BUILD_DIR)/libkilix-mux.a
TEST := $(BUILD_DIR)/test-mux
TAP_TEST := $(BUILD_DIR)/test-tap
FUZZ := $(BUILD_DIR)/fuzz-decoders
BENCH := $(BUILD_DIR)/kmx-bench
SERVE := $(BUILD_DIR)/kmx-serve
SHAPE := $(BUILD_DIR)/kmx-shape
ATTACH := $(BUILD_DIR)/kmx-attach
FLOOD := $(BUILD_DIR)/flood-input

.PHONY: all clean test sanitize fuzz backpressure churn check-vendor install

all: $(STATIC_LIB) $(BENCH) $(SERVE) $(ATTACH) $(SHAPE)

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/src/%.o: src/%.c include/kilix_mux.h | $(BUILD_DIR)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/$(VTERM_DIR)/src/%.o: $(VTERM_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p "$(dir $@)"
	$(CC) -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src $(VTERM_CFLAGS) -c "$<" -o "$@"

$(STATIC_LIB): $(LIB_OBJECTS) $(VTERM_OBJECTS)
	$(AR) rcs "$@" $(LIB_OBJECTS) $(VTERM_OBJECTS)

$(BUILD_DIR)/kmx_bench.o: tools/kmx_bench.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BENCH): $(BUILD_DIR)/kmx_bench.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_bench.o $(STATIC_LIB) $(LDLIBS) -lutil

$(BUILD_DIR)/kmx_serve.o: tools/kmx_serve.c tools/kmx_pixel.h tools/kmx_tap.h include/kilix_mux.h | $(BUILD_DIR)
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

$(SERVE): $(BUILD_DIR)/kmx_serve.o $(BUILD_DIR)/kmx_pixel.o $(BUILD_DIR)/kmx_tap.o $(BUILD_DIR)/kmx_tls.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_serve.o $(BUILD_DIR)/kmx_pixel.o $(BUILD_DIR)/kmx_tap.o $(BUILD_DIR)/kmx_tls.o $(STATIC_LIB) $(LDLIBS) -lutil -lssl -lcrypto

$(BUILD_DIR)/kmx_attach.o: tools/kmx_attach.c tools/kmx_tls.h include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(ATTACH): $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_tls.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_attach.o $(BUILD_DIR)/kmx_tls.o $(STATIC_LIB) $(LDLIBS) -lssl -lcrypto

$(BUILD_DIR)/test_mux.o: tests/test_mux.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(TEST): $(BUILD_DIR)/test_mux.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_mux.o $(STATIC_LIB) $(LDLIBS)

$(BUILD_DIR)/test_tap.o: tests/test_tap.c tools/kmx_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools -c "$<" -o "$@"

$(TAP_TEST): $(BUILD_DIR)/test_tap.o $(BUILD_DIR)/kmx_tap.o
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_tap.o $(BUILD_DIR)/kmx_tap.o

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

test: $(TEST) $(TAP_TEST)
	$(TEST_ENVIRONMENT) "$(TEST)"
	$(TEST_ENVIRONMENT) "$(TAP_TEST)"

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

$(BUILD_DIR)/seed_corpus.o: tests/seed_corpus.c include/kilix_mux.h | $(BUILD_DIR)
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

# The vendored tree is unmodified upstream; prove it rather than assume it.
check-vendor:
	@sha256sum --quiet -c $(VTERM_DIR)/SHA256SUMS \
		&& echo "third_party/libvterm matches its recorded checksums"

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
	install -m 0644 include/kilix_mux.h "$(DESTDIR)$(PREFIX)/include/"
	install -m 0644 "$(STATIC_LIB)" "$(DESTDIR)$(PREFIX)/lib/"
