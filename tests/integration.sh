#!/usr/bin/env bash
#
# End-to-end: a real pane under kmx-serve, a real client under kmx-attach,
# talking over a real socket.  The unit tests prove the encoding; this proves
# the parts actually fit together.
#
# Not part of `make test` because it spawns processes and waits on wall-clock
# time, which does not belong in a suite that must be fast and deterministic.
set -u

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
serve="$root/build/kmx-serve"
attach="$root/build/kmx-attach"
work=$(mktemp -d /tmp/kmxit.XXXXXX)
failures=0

cleanup() {
    pkill -f "kmx-serve --socket $work" 2>/dev/null
    rm -rf -- "$work"
}
trap cleanup EXIT

for binary in "$serve" "$attach"; do
    [ -x "$binary" ] || { echo "build first (make)" >&2; exit 2; }
done

report() {
    if [ "$1" = pass ]; then
        printf 'pass  %s\n' "$2"
    else
        printf 'FAIL  %s\n' "$2"
        failures=$((failures + 1))
    fi
}

# Strip the escape sequences so assertions are about what a person would see.
visible() {
    sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[()][A-Z0-9]//g' "$1" | tr -d '\r'
}

start_pane() {
    local name=$1; shift
    "$serve" --socket "$work/$name.sock" --rows 12 --cols 60 -- "$@" \
        >/dev/null 2>&1 &
    local attempt
    for attempt in $(seq 1 40); do
        [ -S "$work/$name.sock" ] && return 0
        sleep 0.1
    done
    return 1
}

# --- the pane's output reaches the client ---------------------------------
if start_pane basic /bin/sh -c 'printf "PANE_OUTPUT_MARKER\r\n"; sleep 8'; then
    sleep 0.5
    timeout 6 "$attach" --socket "$work/basic.sock" --dump --seconds 2 \
        > "$work/basic.out" 2>&1
    if visible "$work/basic.out" | grep -q PANE_OUTPUT_MARKER; then
        report pass "pane output reaches the client"
    else
        report fail "pane output reaches the client"
    fi
else
    report fail "could not start the basic pane"
fi

# --- a client attaching late still sees what it missed ---------------------
if start_pane late /bin/sh -c 'printf "EARLY_OUTPUT\r\n"; sleep 8'; then
    # Deliberately attach well after the pane printed: the screen is state,
    # not a stream, so history is not something the client had to be present
    # for.
    sleep 2
    timeout 6 "$attach" --socket "$work/late.sock" --dump --seconds 2 \
        > "$work/late.out" 2>&1
    if visible "$work/late.out" | grep -q EARLY_OUTPUT; then
        report pass "a late client still sees earlier output"
    else
        report fail "a late client still sees earlier output"
    fi
else
    report fail "could not start the late pane"
fi

# --- input travels to the pane and its effect comes back -------------------
if start_pane input /bin/sh -c 'stty -echo; while IFS= read -r line; do printf "GOT[%s]\r\n" "$line"; done'; then
    sleep 0.5
    timeout 8 "$attach" --socket "$work/input.sock" --dump --seconds 3 \
        --send 'ROUNDTRIP
' > "$work/input.out" 2>&1
    if visible "$work/input.out" | grep -q 'GOT\[ROUNDTRIP\]'; then
        report pass "client input reaches the pane and its output returns"
    else
        report fail "client input reaches the pane and its output returns"
    fi
else
    report fail "could not start the input pane"
fi

# --- prediction changes latency, never the final screen --------------------
if start_pane predict /bin/sh -c 'stty -echo; while IFS= read -r line; do printf "ECHO[%s]\r\n" "$line"; done'; then
    sleep 0.5
    timeout 8 "$attach" --socket "$work/predict.sock" --dump --seconds 3 \
        --send 'WITHPRED
' > "$work/predict.on" 2>&1
    timeout 8 "$attach" --socket "$work/predict.sock" --dump --seconds 3 \
        --no-predict --send 'NOPRED
' > "$work/predict.off" 2>&1
    if visible "$work/predict.on" | grep -q 'ECHO\[WITHPRED\]' &&
       visible "$work/predict.off" | grep -q 'ECHO\[NOPRED\]'; then
        report pass "predicted and unpredicted clients both converge"
    else
        report fail "predicted and unpredicted clients both converge"
    fi
else
    report fail "could not start the predict pane"
fi

# --- a pane that exits tells the client ------------------------------------
if start_pane quick /bin/sh -c 'printf "DONE_MARKER\r\n"'; then
    sleep 0.3
    start=$(date +%s)
    timeout 10 "$attach" --socket "$work/quick.sock" --dump --seconds 8 \
        > "$work/quick.out" 2>&1
    elapsed=$(( $(date +%s) - start ))
    # The client must leave because the server said the pane ended, well
    # before its own timeout would have fired.
    if [ "$elapsed" -lt 7 ]; then
        report pass "the client leaves when the pane exits"
    else
        report fail "the client leaves when the pane exits (waited ${elapsed}s)"
    fi
else
    report fail "could not start the quick pane"
fi

# --- several panes on one screen, with chrome the client draws itself ------
"$serve" --socket "$work/multi.sock" --rows 20 --cols 100 --split horizontal \
    --pane 'printf "PANE_ALPHA\r\n"; sleep 10' \
    --pane 'printf "PANE_BETA\r\n"; sleep 10' \
    >/dev/null 2>&1 &
for attempt in $(seq 1 40); do [ -S "$work/multi.sock" ] && break; sleep 0.1; done
if [ -S "$work/multi.sock" ]; then
    sleep 0.6
    timeout 8 "$attach" --socket "$work/multi.sock" --dump --seconds 3 \
        > "$work/multi.out" 2>&1
    text=$(visible "$work/multi.out")
    if printf '%s' "$text" | grep -q PANE_ALPHA &&
       printf '%s' "$text" | grep -q PANE_BETA; then
        report pass "two panes both reach the client"
    else
        report fail "two panes both reach the client"
    fi
    # The divider and the pane titles are drawn locally from the layout; they
    # are never sent as content, so finding them proves the layout plane
    # arrived and was used.
    if printf '%s' "$text" | grep -q '│'; then
        report pass "the client draws its own divider from the layout"
    else
        report fail "the client draws its own divider from the layout"
    fi
    if printf '%s' "$text" | grep -q '1: printf' &&
       printf '%s' "$text" | grep -q '2: printf'; then
        report pass "the client draws its own pane titles"
    else
        report fail "the client draws its own pane titles"
    fi
else
    report fail "could not start the multi-pane session"
fi

# --- input goes to the focused pane, and only to it ------------------------
"$serve" --socket "$work/focus.sock" --rows 20 --cols 100 --split horizontal \
    --pane 'stty -echo; while IFS= read -r l; do printf "ONE<%s>\r\n" "$l"; done' \
    --pane 'stty -echo; while IFS= read -r l; do printf "TWO<%s>\r\n" "$l"; done' \
    >/dev/null 2>&1 &
for attempt in $(seq 1 40); do [ -S "$work/focus.sock" ] && break; sleep 0.1; done
if [ -S "$work/focus.sock" ]; then
    sleep 0.6
    timeout 8 "$attach" --socket "$work/focus.sock" --dump --seconds 3 \
        --send 'TYPED
' > "$work/focus.out" 2>&1
    text=$(visible "$work/focus.out")
    if printf '%s' "$text" | grep -q 'ONE<TYPED>' &&
       ! printf '%s' "$text" | grep -q 'TWO<TYPED>'; then
        report pass "input reaches the focused pane and not the other"
    else
        report fail "input reaches the focused pane and not the other"
    fi
else
    report fail "could not start the focus session"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all integration checks passed"
    exit 0
fi
echo "$failures integration check(s) failed" >&2
exit 1
