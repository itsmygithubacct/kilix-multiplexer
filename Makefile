CC ?= cc
AR ?= ar
BUILD_DIR ?= build
PREFIX ?= /usr/local

VTERM_DIR := third_party/libvterm

CPPFLAGS += -D_FORTIFY_SOURCE=2 -Iinclude -Isrc -I$(VTERM_DIR)/include
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC
LDFLAGS ?=
LDLIBS += -lzstd -lm

# Vendored verbatim; built with the upstream project's own warning posture
# rather than ours, so our -Werror never depends on someone else's code.
VTERM_CFLAGS := -O2 -std=c99 -fPIC -w

LIB_SOURCES := src/grid.c src/codec.c src/term.c src/sync.c src/frame.c src/render.c src/predict.c src/layout.c src/graphics.c src/endpoint.c
LIB_OBJECTS := $(LIB_SOURCES:%.c=$(BUILD_DIR)/%.o)
VTERM_SOURCES := $(wildcard $(VTERM_DIR)/src/*.c)
VTERM_OBJECTS := $(VTERM_SOURCES:%.c=$(BUILD_DIR)/%.o)

STATIC_LIB := $(BUILD_DIR)/libkilix-mux.a
TEST := $(BUILD_DIR)/test-mux
FUZZ_CELLS := $(BUILD_DIR)/fuzz-cells
BENCH := $(BUILD_DIR)/kmx-bench
SERVE := $(BUILD_DIR)/kmx-serve
ATTACH := $(BUILD_DIR)/kmx-attach

.PHONY: all clean test sanitize fuzz check-vendor install

all: $(STATIC_LIB) $(BENCH) $(SERVE) $(ATTACH)

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

$(BUILD_DIR)/kmx_serve.o: tools/kmx_serve.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(SERVE): $(BUILD_DIR)/kmx_serve.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_serve.o $(STATIC_LIB) $(LDLIBS) -lutil

$(BUILD_DIR)/kmx_attach.o: tools/kmx_attach.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(ATTACH): $(BUILD_DIR)/kmx_attach.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/kmx_attach.o $(STATIC_LIB) $(LDLIBS)

$(BUILD_DIR)/test_mux.o: tests/test_mux.c include/kilix_mux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(TEST): $(BUILD_DIR)/test_mux.o $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o "$@" $(BUILD_DIR)/test_mux.o $(STATIC_LIB) $(LDLIBS)

test: $(TEST)
	$(TEST_ENVIRONMENT) "$(TEST)"

TEST_ENVIRONMENT ?=

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -fsanitize=address,undefined" \
		VTERM_CFLAGS="-O1 -g -std=c99 -fPIC -w -fsanitize=address,undefined" \
		LDFLAGS="-fsanitize=address,undefined" \
		TEST_ENVIRONMENT="UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" \
		test

# The cell decoder is the parser that faces the network. It is fuzzed rather
# than merely tested; requires a clang with libFuzzer.
fuzz: $(BUILD_DIR)
	clang -std=c11 $(CPPFLAGS) -O1 -g -fsanitize=fuzzer,address,undefined \
		-o "$(FUZZ_CELLS)" tests/fuzz_cells.c $(LIB_SOURCES) $(VTERM_SOURCES) $(LDLIBS)
	"$(FUZZ_CELLS)" -max_total_time=$${FUZZ_SECONDS:-30} -print_final_stats=1

# The vendored tree is unmodified upstream; prove it rather than assume it.
check-vendor:
	@sha256sum --quiet -c $(VTERM_DIR)/SHA256SUMS \
		&& echo "third_party/libvterm matches its recorded checksums"

clean:
	rm -rf -- "$(BUILD_DIR)"

install: all
	install -d "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib"
	install -m 0644 include/kilix_mux.h "$(DESTDIR)$(PREFIX)/include/"
	install -m 0644 "$(STATIC_LIB)" "$(DESTDIR)$(PREFIX)/lib/"
