#!/bin/sh
# Abandon connections at every stage of the handshake, under ASan.
#
# The connection state machine is the part of the server a stranger touches
# first and the part reading is worst at checking: a client moves through
# accept, TLS handshake, greeting and teardown, and the interesting bugs are
# in what happens when it stops partway. So this drives it through eight
# abandonment points a few hundred times each and lets the sanitizer decide.
#
# Two things this got wrong before they were fixed, both worth knowing about
# if the numbers ever look strange:
#
#   - Unpaced, 2968 of 3000 connections were refused by the accept rate
#     limiter, so the run exercised almost nothing while appearing to pass.
#     The client paces itself to the server's allowance; if the server reports
#     refusals at shutdown, the pacing is wrong and the result means little.
#   - "No sanitizer report" is only evidence if the sanitizer would have
#     reported. This builds a deliberately leaky program first and checks that
#     LeakSanitizer catches it, before believing the silence.
set -eu

cd "$(dirname "$0")/.."

command -v python3 >/dev/null || { echo "skip  churn (needs python3)"; exit 0; }

port=${KMX_CHURN_PORT:-47903}
rounds=${KMX_CHURN_ROUNDS:-1600}
token=aabbccddeeff00112233445566778899
work=$(mktemp -d /tmp/kmx-churn-XXXXXX)
server=""

cleanup() {
    [ -n "$server" ] && kill -9 "$server" 2>/dev/null || true
    rm -rf "$work"
    # Sanitized objects left in build/ break the next ordinary build with
    # undefined __asan_* symbols, which reads as a mysterious link error rather
    # than as leftovers from this script.  Cleaned here rather than only on the
    # success path, because the early exits are exactly when it bites.
    make -s clean >/dev/null 2>&1 || true
}
trap cleanup EXIT

fail() { echo "churn: FAIL - $1" >&2; exit 1; }

# --- is the sanitizer actually watching? ----------------------------------
printf '#include <stdlib.h>\nint main(void){ char *p = malloc(1234); (void)p; return 0; }\n' \
    > "$work/leaky.c"
if ! cc -fsanitize=address -g -o "$work/leaky" "$work/leaky.c" 2>/dev/null; then
    echo "skip  churn (no AddressSanitizer in this toolchain)"
    exit 0
fi
if ! ASAN_OPTIONS=detect_leaks=1 "$work/leaky" 2>&1 | grep -q "LeakSanitizer"; then
    fail "leak detection does not report in this environment, so silence would prove nothing"
fi
echo "churn: leak detection confirmed live"

# --- a sanitized server ----------------------------------------------------
make -s clean >/dev/null
make -s \
    CFLAGS="-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -fsanitize=address,undefined" \
    VTERM_CFLAGS="-O1 -g -std=c99 -fPIC -w -fsanitize=address,undefined" \
    LDFLAGS="-fsanitize=address,undefined" \
    build/kmx-serve >/dev/null 2>&1 || fail "sanitized build failed"

ASAN_OPTIONS="detect_leaks=1:log_path=$work/asan" \
UBSAN_OPTIONS="print_stacktrace=1:log_path=$work/ubsan" \
    build/kmx-serve --socket "127.0.0.1:$port" --token "$token" --tls \
        --rows 24 --cols 80 -- /bin/sh -c 'while :; do sleep 1; done' \
        > "$work/server.log" 2>&1 &
server=$!

waited=0
while ! grep -q "listening on" "$work/server.log" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 100 ] && fail "sanitized server never started"
    sleep 0.2
done

python3 tests/churn.py "$port" "$token" "$rounds" || fail "churn client failed"

kill -0 "$server" 2>/dev/null || fail "the server did not survive the churn"
kill -TERM "$server" 2>/dev/null || true
waited=0
while kill -0 "$server" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 100 ] && fail "server ignored SIGTERM after the churn"
    sleep 0.1
done
server=""

# A refusal count means the client outran the accept allowance and the run
# tested less than it looks like it did.
if grep -q "refused" "$work/server.log"; then
    fail "$(grep refused "$work/server.log") - pacing is wrong, result is not meaningful"
fi

reports=$(ls "$work"/asan.* "$work"/ubsan.* 2>/dev/null | wc -l)
[ "$reports" -eq 0 ] || {
    cat "$work"/asan.* "$work"/ubsan.* 2>/dev/null | head -40
    fail "$reports sanitizer report(s)"
}

echo "churn: $rounds connections abandoned at eight points, no sanitizer findings"
echo "churn: passed"
