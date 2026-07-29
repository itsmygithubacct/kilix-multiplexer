#!/bin/sh
# A pane that stops reading its input must not stop the server.
#
# The pane is put in raw mode with echo off, which is what makes this reachable:
# in canonical mode the tty echoes input back and the server drains the echo, so
# the input queue never fills and a blocking write never blocks.  With echo off
# the queue fills after about a hundred kilobytes.
#
# Measured against the version before the fix, the server stopped serving at
# that point, ignored SIGTERM, and had to be killed.  So this asserts three
# things, in order of how much they mean: that a second client can still attach,
# that the first client is not disconnected for typing, and that the server
# still exits when asked.
set -eu

cd "$(dirname "$0")/.."
socket=$(mktemp -u /tmp/kmx-backpressure-XXXXXX.sock)
log=$(mktemp /tmp/kmx-backpressure-XXXXXX.log)
server=""

cleanup() {
    [ -n "$server" ] && kill -9 "$server" 2>/dev/null || true
    rm -f "$socket" "$log"
}
trap cleanup EXIT

fail() {
    echo "backpressure: FAIL - $1" >&2
    exit 1
}

build/kmx-serve --socket "$socket" -- sh -c 'stty raw -echo; exec sleep 3600' \
    >"$log" 2>&1 &
server=$!

waited=0
while [ ! -S "$socket" ]; do
    waited=$((waited + 1))
    [ "$waited" -gt 50 ] && fail "server never created $socket"
    sleep 0.1
done
sleep 0.5

# Well past what the tty will hold, and past what the server will queue.
build/flood-input "$socket" 16 >/dev/null || fail "flood-input could not connect"

# The whole point: the loop is still running.
if ! timeout 10 build/flood-input "$socket" probe >/dev/null; then
    fail "server stopped serving new clients while a pane was not reading"
fi
echo "backpressure: server still serving after the flood"

kill -TERM "$server" 2>/dev/null || true
waited=0
while kill -0 "$server" 2>/dev/null; do
    waited=$((waited + 1))
    [ "$waited" -gt 50 ] && fail "server ignored SIGTERM - the loop is stuck"
    sleep 0.1
done
server=""
echo "backpressure: server exited on SIGTERM"
echo "backpressure: passed"
