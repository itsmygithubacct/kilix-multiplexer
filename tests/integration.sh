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
    sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[()][A-Z0-9]//g' "${1:--}" | tr -d '\r'
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

# --- a graphics escape survives the hop, and a repeat costs a reference ----
#
# The pane emits the same Kitty graphics sequence three times.  A byte
# forwarder would carry it three times; this carries it once and then refers
# to it, which is the whole point of addressing images by their content.
payload=$(head -c 3000 /dev/zero | tr '\0' 'Q')
if start_pane image /bin/sh -c "
    i=0
    while [ \$i -lt 3 ]; do
        printf '\033_Gi=5,a=T;%s\033\\\\' '$payload'
        printf 'IMAGE_ROUND_%d\r\n' \$i
        i=\$((i+1))
        sleep 0.4
    done
    sleep 6"; then
    timeout 8 "$attach" --socket "$work/image.sock" --dump --seconds 4 \
        > "$work/image.out" 2>&1
    # The graphics escape must reach the client's terminal verbatim.
    if grep -q 'Gi=5,a=T;QQQ' "$work/image.out"; then
        report pass "a graphics escape reaches the client"
    else
        report fail "a graphics escape reaches the client"
    fi
    # Every round must reach the client's terminal, including the ones the
    # server sent as a bare reference: the client is expected to replay them
    # from its cache, so the escape appears in full here each time.
    #
    # Note what this does and does not show.  It measures the client's
    # TERMINAL output, not the socket, so it proves the cache reconstructs a
    # referenced image correctly - not that the wire got smaller.  The wire
    # saving is measured where it can be: test_repeated_image_costs_a_reference
    # in the unit suite.
    # The pane emits exactly three rounds.  Counting rendered text instead
    # would be wrong: a redraw repeats what is on screen, so the same marker
    # legitimately appears more than once.
    payload_hits=$(grep -o 'Gi=5,a=T;QQQ' "$work/image.out" | wc -l)
    if [ "$payload_hits" -ge 3 ]; then
        report pass "referenced images are replayed in full from the client cache"
    else
        report fail "referenced images are replayed in full from the client cache ($payload_hits of 3)"
    fi
else
    report fail "could not start the image pane"
fi

# --- several clients on one session ---------------------------------------
if start_pane shared /bin/sh -c 'printf "SHARED_MARKER\r\n"; sleep 10'; then
    sleep 0.5
    timeout 8 "$attach" --socket "$work/shared.sock" --dump --seconds 3 \
        > "$work/shared.a" 2>&1 &
    first=$!
    timeout 8 "$attach" --socket "$work/shared.sock" --dump --seconds 3 \
        > "$work/shared.b" 2>&1 &
    second=$!
    wait "$first" 2>/dev/null
    wait "$second" 2>/dev/null
    if visible "$work/shared.a" | grep -q SHARED_MARKER &&
       visible "$work/shared.b" | grep -q SHARED_MARKER; then
        report pass "two clients attached at once both see the session"
    else
        report fail "two clients attached at once both see the session"
    fi
else
    report fail "could not start the shared session"
fi

# --- a viewer cannot type, and the server is what stops it -----------------
#
# The viewer sends input anyway; the point is that the server refuses it. A
# client that merely chose not to send would prove nothing.
if start_pane viewer /bin/sh -c 'stty -echo; while IFS= read -r l; do printf "SAW[%s]\r\n" "$l"; done'; then
    sleep 0.5
    timeout 8 "$attach" --socket "$work/viewer.sock" --dump --seconds 3 \
        --view --send 'FROM_VIEWER
' > "$work/viewer.out" 2>&1 &
    watcher=$!
    sleep 0.4
    timeout 8 "$attach" --socket "$work/viewer.sock" --dump --seconds 3 \
        --send 'FROM_CONTROL
' > "$work/control.out" 2>&1
    wait "$watcher" 2>/dev/null
    text=$(visible "$work/control.out")
    if printf '%s' "$text" | grep -q 'SAW\[FROM_CONTROL\]' &&
       ! printf '%s' "$text" | grep -q 'SAW\[FROM_VIEWER\]'; then
        report pass "the server refuses a viewer's input and accepts a controller's"
    else
        report fail "the server refuses a viewer's input and accepts a controller's"
    fi
else
    report fail "could not start the viewer session"
fi

# --- over TCP, which is what makes "remote" mean anything ------------------
port=47$(( ($$ % 900) + 100 ))
"$serve" --socket "127.0.0.1:$port" --rows 12 --cols 60 \
    -- /bin/sh -c 'printf "OVER_TCP_MARKER\r\n"; sleep 10' >/dev/null 2>&1 &
sleep 1
timeout 8 "$attach" --socket "127.0.0.1:$port" --dump --seconds 3 \
    > "$work/tcp.out" 2>&1
if visible "$work/tcp.out" | grep -q OVER_TCP_MARKER; then
    report pass "a session is reachable over TCP"
else
    report fail "a session is reachable over TCP"
fi

# --- a public bind has to be asked for -------------------------------------
#
# The default is loopback, so an ordinary `--socket host:port` cannot put a
# shell on the network by accident.  Refused rather than quietly narrowed: a
# caller that asked for a public address and silently got loopback would
# believe it was reachable when it was not.
out=$("$serve" --socket "0.0.0.0:$((port + 1))" -- /bin/sh -c 'sleep 1' 2>&1)
if printf '%s' "$out" | grep -q 'not a loopback address'; then
    report pass "a non-loopback bind is refused unless asked for"
else
    report fail "a non-loopback bind is refused unless asked for"
fi

# --- a client that stops reading cannot stall anyone else ------------------
#
# The highest-risk failure mode of a fan-out: one peer that never reads must
# not be able to hold up the panes or the other clients.  A client socket that
# was written to blocking would freeze the whole server here.
stall_port=$(( port + 2 ))
"$serve" --socket "127.0.0.1:$stall_port" --rows 12 --cols 60 \
    -- /bin/sh -c 'i=0; while [ $i -lt 400 ]; do printf "flood line %d padded out a bit\r\n" $i; i=$((i+1)); done; printf "FLOOD_DONE\r\n"; sleep 10' \
    >/dev/null 2>&1 &
sleep 0.8
# A peer that connects and then never reads a byte.
( exec 3<>"/dev/tcp/127.0.0.1/$stall_port" || exit 0; sleep 8 ) >/dev/null 2>&1 &
staller=$!
sleep 0.5
timeout 10 "$attach" --socket "127.0.0.1:$stall_port" --dump --seconds 4 \
    > "$work/stall.out" 2>&1
kill "$staller" 2>/dev/null
wait "$staller" 2>/dev/null
if visible "$work/stall.out" | grep -q FLOOD_DONE; then
    report pass "a client that stops reading does not stall the others"
else
    report fail "a client that stops reading does not stall the others"
fi

# --- the client survives the server being restarted under it ---------------
#
# The session is not on the client's side: the panes keep running and their
# screens are state the server can describe again.  So a dropped link should
# be reattached, not fatal.  Here the server process is replaced entirely,
# which is a harsher test than a dropped connection.
rc_port=$(( port + 3 ))
"$serve" --socket "127.0.0.1:$rc_port" --rows 12 --cols 60 \
    -- /bin/sh -c 'printf "BEFORE_DROP\r\n"; sleep 30' >/dev/null 2>&1 &
first_server=$!
sleep 0.8
timeout 20 "$attach" --socket "127.0.0.1:$rc_port" --dump --seconds 12 \
    --reconnect 10 > "$work/reconnect.out" 2>&1 &
reader=$!
sleep 2
kill "$first_server" 2>/dev/null
wait "$first_server" 2>/dev/null
sleep 1
# A new session on the same address; the client should find its way back.
"$serve" --socket "127.0.0.1:$rc_port" --rows 12 --cols 60 \
    -- /bin/sh -c 'printf "AFTER_RECONNECT\r\n"; sleep 20' >/dev/null 2>&1 &
second_server=$!
wait "$reader" 2>/dev/null
kill "$second_server" 2>/dev/null
wait "$second_server" 2>/dev/null
text=$(visible "$work/reconnect.out")
if printf '%s' "$text" | grep -q BEFORE_DROP &&
   printf '%s' "$text" | grep -q AFTER_RECONNECT; then
    report pass "the client reattaches after the link drops"
else
    report fail "the client reattaches after the link drops"
fi

# --- a pixel pane: a real X client, captured and streamed -----------------
#
# The motion plane driven by something real rather than a frame generator.
# The X client runs on a display created for the session, so this can never
# draw on a screen someone is actually using.
if command -v Xvfb >/dev/null && command -v ffmpeg >/dev/null &&
   command -v xclock >/dev/null; then
    pix_port=$(( port + 4 ))
    "$serve" --socket "127.0.0.1:$pix_port" \
        --pixel-pane 'xclock -update 1' --pixel-size 320x240 --pixel-fps 4 \
        >/dev/null 2>&1 &
    sleep 3
    timeout 12 "$attach" --socket "127.0.0.1:$pix_port" --dump --seconds 5 \
        > "$work/pixel.out" 2>&1
    frames=$(grep -c '^KMX_FRAME' "$work/pixel.out")
    if [ "$frames" -ge 2 ]; then
        report pass "a pixel pane streams frames to the client ($frames)"
    else
        report fail "a pixel pane streams frames to the client ($frames)"
    fi
    # A ticking clock must not produce identical frames: equal checksums
    # would mean the decoder was reconstructing the same picture every time,
    # which is what a broken delta path looks like.
    distinct=$(grep '^KMX_FRAME' "$work/pixel.out" | awk '{print $4}' | sort -u | wc -l)
    if [ "$distinct" -ge 2 ]; then
        report pass "successive frames differ, so deltas are being applied"
    else
        report fail "successive frames differ, so deltas are being applied"
    fi
    pkill -f "pixel-pane" 2>/dev/null
else
    echo "skip  pixel pane (needs Xvfb, ffmpeg and xclock)"
fi

# --- the audio plane, end to end ------------------------------------------
#
# A deterministic PCM source rather than a sound card: this is about the
# plane, not about whether the machine has speakers.
"$serve" --socket "$work/audio2.sock" --rows 12 --cols 60 \
    --audio-source 'while :; do head -c 19200 /dev/zero; sleep 0.1; done' \
    --audio-rate 48000 --audio-channels 2 \
    -- /bin/sh -c 'printf "WITH_AUDIO\r\n"; sleep 20' >/dev/null 2>&1 &
for attempt in $(seq 1 40); do [ -S "$work/audio2.sock" ] && break; sleep 0.1; done
if [ -S "$work/audio2.sock" ]; then
    timeout 10 "$attach" --socket "$work/audio2.sock" --dump --seconds 3 \
        > "$work/audio.out" 2>&1
    blocks=$(grep -c '^KMX_AUDIO' "$work/audio.out")
    if [ "$blocks" -ge 10 ]; then
        report pass "audio blocks reach the client ($blocks)"
    else
        report fail "audio blocks reach the client ($blocks)"
    fi
    # 48 kHz stereo 16-bit for 20 ms is exactly 3840 bytes.  A different size
    # would mean the block boundary and the declared format disagree.
    if grep -q '^KMX_AUDIO 3840 ' "$work/audio.out"; then
        report pass "audio block size matches the declared format"
    else
        report fail "audio block size matches the declared format"
    fi
    # Timestamps must advance by the block's own duration, which is what lets
    # a receiver tell a gap from a late delivery.
    if grep -q 'at=.*gap=0' "$work/audio.out"; then
        report pass "audio timestamps run continuously"
    else
        report fail "audio timestamps run continuously"
    fi
else
    report fail "could not start the audio session"
fi

# --- a reachable socket demands a token ------------------------------------
#
# --lan hands the session to whoever can route to the port, so a token is
# mandatory there and cannot be turned off.  Refusal is silent: a peer that
# cannot present it learns only that the connection closed.
tok_port=$(( port + 5 ))
# A genuinely reachable address, since the requirement follows the address
# rather than the flag.  Without one there is nothing to test here.
host_ip=$(ip -4 addr show scope global 2>/dev/null |
          grep -oE 'inet [0-9.]+' | awk '{print $2}' | head -1)
if [ -z "$host_ip" ]; then
    echo "skip  token checks (no non-loopback address)"
    host_ip=""
fi
if [ -n "$host_ip" ]; then
"$serve" --socket "$host_ip:$tok_port" --lan --rows 10 --cols 40 \
    -- /bin/sh -c 'printf "TOKEN_PANE\r\n"; sleep 15' >"$work/token.log" 2>&1 &
sleep 1.5
minted=$(grep -oE -- '--token [0-9a-f]+' "$work/token.log" | awk '{print $2}' | head -1)
if [ -n "$minted" ] && [ "${#minted}" -eq 32 ]; then
    report pass "a token is minted for a reachable socket"
else
    report fail "a token is minted for a reachable socket"
fi
without=$(timeout 8 "$attach" --socket "$host_ip:$tok_port" --dump --seconds 2 2>&1 | visible /dev/stdin | grep -c TOKEN_PANE)
wrong=$(timeout 8 "$attach" --socket "$host_ip:$tok_port" \
    --token 0000000000000000000000000000dead --dump --seconds 2 2>&1 |
    visible /dev/stdin | grep -c TOKEN_PANE)
right=$(timeout 8 "$attach" --socket "$host_ip:$tok_port" \
    --token "$minted" --dump --seconds 3 2>&1 | visible /dev/stdin | grep -c TOKEN_PANE)
if [ "$without" -eq 0 ] && [ "$wrong" -eq 0 ] && [ "$right" -ge 1 ]; then
    report pass "the token is required and checked"
else
    report fail "the token is required and checked (none=$without wrong=$wrong right=$right)"
fi
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all integration checks passed"
    exit 0
fi
echo "$failures integration check(s) failed" >&2
exit 1
