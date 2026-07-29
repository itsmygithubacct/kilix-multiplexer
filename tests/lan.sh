#!/usr/bin/env bash
#
# Direct, cross-machine LAN validation.  Unlike tests/network.sh, this does
# not put the protocol inside an SSH tunnel: SSH is used only to stage the
# tree and start the peer-side process.
#
#   tests/lan.sh REMOTE [REMOTE_PATH]
#
# REMOTE is a passwordless SSH destination.  REMOTE_PATH is a private build
# tree on that machine (default: kmx-lan-test).  The addresses are learned
# from SSH_CONNECTION, so no workstation names or site-specific addresses
# belong in this file.  Override discovery with KMX_LAN_LOCAL_ADDRESS and
# KMX_LAN_REMOTE_ADDRESS when routing requires a different interface.
#
# Not part of `make test`: it needs a second machine and opens reachable,
# authenticated TLS listeners for the duration of the checks.
set -u

remote=${1:-}
remote_path=${2:-kmx-lan-test}
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
serve="$root/build/kmx-serve"
attach="$root/build/kmx-attach"
base_port=${KMX_LAN_PORT:-47820}
failures=0
local_group=
remote_group=
remote_log=
media_client=
work=$(mktemp -d "${TMPDIR:-/tmp}/kmx-lan.XXXXXX") || exit 2
ssh_options=(-o BatchMode=yes -o ConnectTimeout=10)

usage() {
    echo "usage: tests/lan.sh REMOTE [REMOTE_PATH]" >&2
    exit 2
}

die() {
    echo "lan: $*" >&2
    exit 2
}

if [ -z "$remote" ] || [[ "$remote" == -* ]]; then
    usage
fi
if [[ ! "$remote_path" =~ ^[A-Za-z0-9_./~-]+$ ]] ||
   [[ "/$remote_path/" == *"/../"* ]] || [ "$remote_path" = "/" ]; then
    die "REMOTE_PATH must be a narrow path without spaces or '..'"
fi
if [[ ! "$base_port" =~ ^[0-9]+$ ]] ||
   [ "$base_port" -lt 1024 ] || [ "$base_port" -gt 65532 ]; then
    die "KMX_LAN_PORT must leave three usable non-privileged ports"
fi

report() {
    if [ "$1" = pass ]; then
        printf 'pass  %s\n' "$2"
    else
        printf 'FAIL  %s\n' "$2"
        failures=$((failures + 1))
    fi
}

# The dump renderer emits terminal control between individual changed cells.
# Remove it before making assertions about what a person would read.
visible() {
    sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[()][A-Z0-9]//g' |
        tr -d '\r'
}

random_hex() {
    od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

fingerprint_from() {
    grep -oE -- '--tls-fingerprint [0-9a-f]{64}' "$1" 2>/dev/null |
        awk '{print $2}' | head -1
}

stop_local() {
    local pid=${local_group:-}
    local pgid
    [ -n "$pid" ] || return 0
    pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
    if [ "$pgid" = "$pid" ]; then
        kill -TERM -- "-$pid" 2>/dev/null || true
    else
        kill -TERM "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    local_group=
}

stop_remote() {
    local pid=${remote_group:-}
    local log=${remote_log:-}
    [ -n "$pid" ] || return 0
    ssh "${ssh_options[@]}" "$remote" bash -s -- \
        "$pid" "$remote_path" "$log" <<'REMOTE' >/dev/null 2>&1
pid=$1
root=$2
log=$3
case "$pid" in ''|*[!0-9]*) exit 2;; esac
pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
if [ "$pgid" = "$pid" ]; then
    kill -TERM -- "-$pid" 2>/dev/null || true
else
    kill -TERM "$pid" 2>/dev/null || true
fi
if cd -- "$root" 2>/dev/null; then
    case "$log" in build/.lan-*.log) rm -f -- "$log";; esac
fi
REMOTE
    remote_group=
    remote_log=
}

# Invoked through trap rather than an ordinary call on every exit path.
# shellcheck disable=SC2317
cleanup() {
    if [ -n "${media_client:-}" ]; then
        kill "$media_client" 2>/dev/null || true
    fi
    stop_local
    stop_remote
    case "$work" in
        "${TMPDIR:-/tmp}"/kmx-lan.*) rm -r -- "$work";;
    esac
}
trap cleanup EXIT INT TERM

read_remote_log() {
    ssh "${ssh_options[@]}" "$remote" bash -s -- \
        "$remote_path" "$remote_log" <<'REMOTE' 2>/dev/null
cd -- "$1" || exit 2
case "$2" in
    build/.lan-*.log) [ ! -f "$2" ] || cat -- "$2";;
    *) exit 2;;
esac
REMOTE
}

remote_alive() {
    ssh "${ssh_options[@]}" "$remote" bash -s -- "$remote_group" <<'REMOTE' \
        >/dev/null 2>&1
case "$1" in ''|*[!0-9]*) exit 2;; esac
kill -0 "$1"
REMOTE
}

wait_local_fingerprint() {
    local log=$1
    local attempt fp
    for attempt in $(seq 1 80); do
        fp=$(fingerprint_from "$log")
        if [ "${#fp}" -eq 64 ]; then
            printf '%s\n' "$fp"
            return 0
        fi
        kill -0 "$local_group" 2>/dev/null || break
        sleep 0.1
    done
    return 1
}

wait_remote_fingerprint() {
    local attempt=0
    local text fp
    while [ "$attempt" -lt 30 ]; do
        attempt=$((attempt + 1))
        text=$(read_remote_log)
        fp=$(printf '%s\n' "$text" |
            grep -oE -- '--tls-fingerprint [0-9a-f]{64}' |
            awk '{print $2}' | head -1)
        if [ "${#fp}" -eq 64 ]; then
            printf '%s\n' "$fp"
            return 0
        fi
        remote_alive || break
        sleep 0.2
    done
    return 1
}

remote_attach() {
    local address=$1
    local port=$2
    local token=$3
    local fingerprint=$4
    local seconds=$5
    local send=${6:--}
    local role=${7:-control}
    ssh "${ssh_options[@]}" "$remote" bash -s -- \
        "$remote_path" "$address" "$port" "$token" "$fingerprint" \
        "$seconds" "$send" "$role" <<'REMOTE'
root=$1
address=$2
port=$3
token=$4
fingerprint=$5
seconds=$6
send=$7
role=$8
command=("$root/build/kmx-attach" --socket "$address:$port"
         --dump --seconds "$seconds" --reconnect 0)
[ "$token" = - ] || command+=(--token "$token")
[ "$fingerprint" = - ] || command+=(--tls-fingerprint "$fingerprint")
[ "$role" != view ] || command+=(--view)
[ "$send" = - ] || command+=(--send "$send"$'\n')
timeout "$((seconds + 8))" "${command[@]}" 2>&1
REMOTE
}

local_attach() {
    local address=$1
    local port=$2
    local token=$3
    local fingerprint=$4
    local seconds=$5
    local send=${6:--}
    local role=${7:-control}
    local command=("$attach" --socket "$address:$port"
                   --dump --seconds "$seconds" --reconnect 0)
    [ "$token" = - ] || command+=(--token "$token")
    [ "$fingerprint" = - ] || command+=(--tls-fingerprint "$fingerprint")
    [ "$role" != view ] || command+=(--view)
    [ "$send" = - ] || command+=(--send "$send"$'\n')
    timeout "$((seconds + 8))" "${command[@]}" 2>&1
}

start_local_text_server() {
    local address=$1
    local port=$2
    local token=$3
    local marker=$4
    local log=$5
    local pgid
    # The variables belong to the child shell, not this script.
    # shellcheck disable=SC2016
    setsid "$serve" --socket "$address:$port" --lan --token "$token" \
        --rows 12 --cols 60 -- \
        /bin/sh -c 'stty -echo; printf "%s\r\n" "$1"; while IFS= read -r line; do printf "ECHO[%s]\r\n" "$line"; done' \
        sh "$marker" >"$log" 2>&1 </dev/null &
    local_group=$!
    sleep 0.1
    pgid=$(ps -o pgid= -p "$local_group" 2>/dev/null | tr -d ' ')
    [ "$pgid" = "$local_group" ] ||
        die "local server did not enter its own process group"
}

start_remote_text_server() {
    local address=$1
    local port=$2
    local token=$3
    local marker=$4
    local run_id=$5
    local record
    record=$(ssh "${ssh_options[@]}" "$remote" bash -s -- \
        "$remote_path" "$address" "$port" "$token" "$marker" "$run_id" <<'REMOTE'
root=$1
address=$2
port=$3
token=$4
marker=$5
run_id=$6
cd -- "$root" || exit 2
log="build/.lan-${run_id}-remote.log"
setsid ./build/kmx-serve --socket "$address:$port" --lan --token "$token" \
    --rows 12 --cols 60 -- \
    /bin/sh -c 'stty -echo; printf "%s\r\n" "$1"; while IFS= read -r line; do printf "ECHO[%s]\r\n" "$line"; done' \
    sh "$marker" >"$log" 2>&1 </dev/null &
pid=$!
sleep 0.1
pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
if [ "$pgid" != "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    exit 1
fi
printf '%s %s\n' "$pid" "$log"
REMOTE
) || die "could not start the remote server"
    read -r remote_group remote_log <<<"$record"
    if [[ ! "$remote_group" =~ ^[0-9]+$ ]] ||
       [[ ! "$remote_log" =~ ^build/\.lan-[A-Za-z0-9_-]+-remote\.log$ ]]; then
        remote_group=
        remote_log=
        die "remote server returned an invalid process record"
    fi
}

start_local_media_server() {
    local address=$1
    local port=$2
    local token=$3
    local tap_session=$4
    local log=$5
    local pgid
    setsid "$serve" --socket "$address:$port" --lan --token "$token" \
        --rows 12 --cols 60 \
        --tap-socket "$work/presenter.tap" --tap-session "$tap_session" \
        --audio-source 'while :; do head -c 3840 /dev/zero; sleep 0.02; done' \
        --audio-rate 48000 --audio-channels 2 \
        -- /bin/sh -c 'printf "MEDIA_PANE\r\n"; sleep 20' \
        >"$log" 2>&1 </dev/null &
    local_group=$!
    sleep 0.1
    pgid=$(ps -o pgid= -p "$local_group" 2>/dev/null | tr -d ' ')
    [ "$pgid" = "$local_group" ] ||
        die "local media server did not enter its own process group"
}

send_tapped_frames() {
    local socket_path=$1
    local session=$2
    python3 - "$socket_path" "$session" <<'PY'
import socket
import struct
import sys
import time

path, session = sys.argv[1:3]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.monotonic() + 5
while True:
    try:
        client.connect(path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        if time.monotonic() >= deadline:
            raise
        time.sleep(0.05)

width, height = 64, 48
for number in range(30):
    rgb = bytes((index + number * 17) & 255
                for index in range(width * height * 3))
    header = struct.pack(
        "!4sHH64sIIIIiiQQ",
        b"KFT1", 1, 0, session.encode("ascii"),
        width, height, 20, 8, 0, 0,
        time.monotonic_ns() // 1000, len(rgb),
    )
    client.sendall(header + rgb)
    time.sleep(0.08)
client.close()
PY
}

connection=$(ssh "${ssh_options[@]}" "$remote" \
    'printf "%s\n" "$SSH_CONNECTION"') ||
    die "could not inspect the SSH connection"
read -r discovered_local _ discovered_remote _ <<<"$connection"
local_address=${KMX_LAN_LOCAL_ADDRESS:-$discovered_local}
remote_address=${KMX_LAN_REMOTE_ADDRESS:-$discovered_remote}

for address in "$local_address" "$remote_address"; do
    if [[ ! "$address" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
       [[ "$address" == 127.* ]] || [ "$address" = "0.0.0.0" ]; then
        die "direct LAN checks currently require two non-loopback IPv4 addresses"
    fi
done

echo "local source:  $local_address"
echo "remote source: $remote_address"
echo "remote tree:   $remote_path"

make -C "$root" --silent >/dev/null ||
    die "local build failed"
if ! ssh "${ssh_options[@]}" "$remote" bash -s -- "$remote_path" <<'REMOTE'
case "$1" in /|'') exit 2;; esac
mkdir -p -- "$1"
REMOTE
then
    die "could not create the remote build tree"
fi
rsync -a --exclude build --exclude .git "$root/" "$remote:$remote_path/" ||
    die "could not synchronise the source tree"
if ! ssh "${ssh_options[@]}" "$remote" bash -s -- "$remote_path" \
    >/dev/null <<'REMOTE'
cd -- "$1" && make --silent
REMOTE
then
    die "remote build failed"
fi
echo "built on both endpoints"
echo

token=$(random_hex)
wrong_token=$(random_hex)
run_id=${token:0:12}
bad_fingerprint=$(printf '0%.0s' $(seq 1 64))

# A local session, controlled directly by the peer.
local_screen="LOCAL_SCREEN_$run_id"
remote_input="REMOTE_INPUT_$run_id"
local_log="$work/local.log"
start_local_text_server \
    "$local_address" "$base_port" "$token" "$local_screen" "$local_log"
local_fingerprint=$(wait_local_fingerprint "$local_log") ||
    die "local TLS listener did not become ready"

output=$(remote_attach "$local_address" "$base_port" "$token" \
    "$local_fingerprint" 4 "$remote_input")
text=$(visible <<<"$output")
if grep -Fq "$local_screen" <<<"$text"; then
    report pass "peer renders a session served here over direct TLS"
else
    report fail "peer renders a session served here over direct TLS"
fi
if grep -Fq "ECHO[$remote_input]" <<<"$text"; then
    report pass "peer input reaches the local pane and returns"
else
    report fail "peer input reaches the local pane and returns"
fi

view_input="VIEW_INPUT_$run_id"
view_output=$(remote_attach "$local_address" "$base_port" "$token" \
    "$local_fingerprint" 3 "$view_input" view)
view_text=$(visible <<<"$view_output")
if grep -Fq "$local_screen" <<<"$view_text" &&
   ! grep -Fq "ECHO[$view_input]" <<<"$view_text"; then
    report pass "a view attachment cannot inject input"
else
    report fail "a view attachment cannot inject input"
fi

wrong_output=$(remote_attach "$local_address" "$base_port" "$wrong_token" \
    "$local_fingerprint" 2)
bad_pin_output=$(remote_attach "$local_address" "$base_port" "$token" \
    "$bad_fingerprint" 2)
plain_output=$(remote_attach "$local_address" "$base_port" "$token" - 2)
wrong_text=$(visible <<<"$wrong_output")
bad_pin_text=$(visible <<<"$bad_pin_output")
plain_text=$(visible <<<"$plain_output")
if ! grep -Fq "$local_screen" <<<"$wrong_text"; then
    report pass "a wrong token sees no session data"
else
    report fail "a wrong token sees no session data"
fi
if ! grep -Fq "$local_screen" <<<"$bad_pin_text" &&
   ! grep -Fq "$local_screen" <<<"$plain_text"; then
    report pass "a bad TLS pin and plaintext fallback both fail closed"
else
    report fail "a bad TLS pin and plaintext fallback both fail closed"
fi

sleep 2
survival_input="AFTER_REFUSAL_$run_id"
survival=$(remote_attach "$local_address" "$base_port" "$token" \
    "$local_fingerprint" 3 "$survival_input")
survival_text=$(visible <<<"$survival")
if kill -0 "$local_group" 2>/dev/null &&
   grep -Fq "ECHO[$survival_input]" <<<"$survival_text"; then
    report pass "refused peers do not kill the reachable server"
else
    report fail "refused peers do not kill the reachable server"
fi
stop_local

# A peer-side session, controlled directly from here.
remote_screen="REMOTE_SCREEN_$run_id"
local_input="LOCAL_INPUT_$run_id"
start_remote_text_server \
    "$remote_address" "$((base_port + 1))" "$token" \
    "$remote_screen" "$run_id"
remote_fingerprint=$(wait_remote_fingerprint) ||
    die "remote TLS listener did not become ready"
output=$(local_attach "$remote_address" "$((base_port + 1))" "$token" \
    "$remote_fingerprint" 4 "$local_input")
text=$(visible <<<"$output")
if grep -Fq "$remote_screen" <<<"$text"; then
    report pass "this endpoint renders a peer-side session over direct TLS"
else
    report fail "this endpoint renders a peer-side session over direct TLS"
fi
if grep -Fq "ECHO[$local_input]" <<<"$text"; then
    report pass "local input reaches the peer-side pane and returns"
else
    report fail "local input reaches the peer-side pane and returns"
fi
if remote_alive; then
    report pass "peer-side server remains attached after the client leaves"
else
    report fail "peer-side server remains attached after the client leaves"
fi
stop_remote

# Presenter frames and audio share the direct LAN transport but keep their own
# droppable timing semantics.  The dump client records checksums and block
# metadata, which makes the assertions independent of either machine's display
# or sound-server configuration.
media_log="$work/media.log"
media_output="$work/media.out"
tap_session=${token:0:16}
start_local_media_server \
    "$local_address" "$((base_port + 2))" "$token" "$tap_session" "$media_log"
media_fingerprint=$(wait_local_fingerprint "$media_log") ||
    die "media TLS listener did not become ready"
remote_attach "$local_address" "$((base_port + 2))" "$token" \
    "$media_fingerprint" 6 >"$media_output" &
media_client=$!
sleep 1
if ! send_tapped_frames "$work/presenter.tap" "$tap_session"; then
    report fail "presenter producer connected to the frame tap"
fi
wait "$media_client" 2>/dev/null || true
media_client=

frames=$(grep -c '^KMX_FRAME 64x48 ' "$media_output" 2>/dev/null || true)
distinct=$(grep '^KMX_FRAME 64x48 ' "$media_output" 2>/dev/null |
    awk '{print $4}' | sort -u | wc -l)
audio=$(grep -c '^KMX_AUDIO 3840 ' "$media_output" 2>/dev/null || true)
if [ "$frames" -ge 2 ] && [ "$distinct" -ge 2 ]; then
    report pass "presenter tap streams changing RGB frames to the peer ($frames)"
else
    report fail "presenter tap streams changing RGB frames to the peer ($frames)"
fi
if [ "$audio" -ge 2 ] &&
   grep -q '^KMX_AUDIO .*gap=0$' "$media_output"; then
    report pass "PCM audio reaches the peer with continuous timestamps ($audio)"
else
    report fail "PCM audio reaches the peer with continuous timestamps ($audio)"
fi
stop_local

echo
if [ "$failures" -eq 0 ]; then
    echo "all direct LAN checks passed"
    exit 0
fi
echo "$failures direct LAN check(s) failed" >&2
exit 1
