#!/usr/bin/env bash
#
# Cross-machine checks: a session on one host, a client on another, over an
# SSH tunnel to a loopback port - which is the intended way to reach a session
# across a network and the reason TCP binds loopback by default.
#
#   tests/network.sh REMOTE [REMOTE_PATH]
#
# REMOTE is an ssh destination that can be reached without a password.
# REMOTE_PATH is where this tree lives on it (default kmx-test); it is
# synchronised and built before the checks run.
#
# Not part of `make test`: it needs a second machine.
set -u

remote=${1:-}
remote_path=${2:-kmx-test}
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
failures=0
base_port=${KMX_NET_PORT:-47800}

if [ -z "$remote" ]; then
    echo "usage: tests/network.sh REMOTE [REMOTE_PATH]" >&2
    exit 2
fi

cleanup() {
    pkill -x kmx-serve 2>/dev/null
    ssh -o BatchMode=yes "$remote" 'pkill -x kmx-serve 2>/dev/null' 2>/dev/null
    [ -n "${tunnel:-}" ] && kill "$tunnel" 2>/dev/null
    return 0
}
trap cleanup EXIT

report() {
    if [ "$1" = pass ]; then
        printf 'pass  %s\n' "$2"
    else
        printf 'FAIL  %s\n' "$2"
        failures=$((failures + 1))
    fi
}

visible() {
    sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -d '\r'
}

echo "local:  $root"
echo "remote: $remote:$remote_path"
make -C "$root" --silent >/dev/null || { echo "local build failed" >&2; exit 2; }
rsync -a --delete --exclude build --exclude .git "$root/" "$remote:$remote_path/" \
    || { echo "could not synchronise to $remote" >&2; exit 2; }
ssh -o BatchMode=yes "$remote" "cd '$remote_path' && make --silent" >/dev/null 2>&1 \
    || { echo "remote build failed" >&2; exit 2; }
echo "built on both"
echo

# --- a session here, a client there ----------------------------------------
here_port=$base_port
there_port=$((base_port + 1))
setsid "$root/build/kmx-serve" --socket "127.0.0.1:$here_port" --rows 12 --cols 60 \
    -- /bin/sh -c 'stty -echo; printf "LOCAL_PANE\r\n"; while IFS= read -r l; do printf "ECHO[%s]\r\n" "$l"; done' \
    >/dev/null 2>&1 &
sleep 1.5
# -R publishes a port on the remote that reaches this machine's loopback.
out=$(timeout 60 ssh -o BatchMode=yes -o ExitOnForwardFailure=yes \
    -R "$there_port:127.0.0.1:$here_port" "$remote" \
    "'$remote_path/build/kmx-attach' --socket 127.0.0.1:$there_port --dump --seconds 4 --send 'FROM_REMOTE
'" 2>/dev/null | visible)
if printf '%s' "$out" | grep -q LOCAL_PANE; then
    report pass "a remote client renders a session running here"
else
    report fail "a remote client renders a session running here"
fi
if printf '%s' "$out" | grep -q 'ECHO\[FROM_REMOTE\]'; then
    report pass "a remote client's typing reaches the pane and returns"
else
    report fail "a remote client's typing reaches the pane and returns"
fi
pkill -x kmx-serve 2>/dev/null
sleep 0.5

# --- a session there, a client here ----------------------------------------
remote_port=$((base_port + 2))
local_port=$((base_port + 3))
ssh -o BatchMode=yes "$remote" \
    "pkill -x kmx-serve 2>/dev/null; setsid nohup '$remote_path/build/kmx-serve' --socket 127.0.0.1:$remote_port --rows 12 --cols 60 -- /bin/sh -c 'stty -echo; printf \"REMOTE_PANE\r\n\"; while IFS= read -r l; do printf \"ECHO[%s]\r\n\" \"\$l\"; done' >/dev/null 2>&1 </dev/null & sleep 1" \
    >/dev/null 2>&1
ssh -o BatchMode=yes -o ExitOnForwardFailure=yes \
    -L "$local_port:127.0.0.1:$remote_port" -N "$remote" &
tunnel=$!
sleep 2
out=$(timeout 30 "$root/build/kmx-attach" --socket "127.0.0.1:$local_port" \
    --dump --seconds 4 --send 'FROM_LOCAL
' 2>/dev/null | visible)
kill "$tunnel" 2>/dev/null
tunnel=""
if printf '%s' "$out" | grep -q REMOTE_PANE; then
    report pass "a client here renders a session running remotely"
else
    report fail "a client here renders a session running remotely"
fi
if printf '%s' "$out" | grep -q 'ECHO\[FROM_LOCAL\]'; then
    report pass "typing here reaches the remote pane and returns"
else
    report fail "typing here reaches the remote pane and returns"
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "all cross-machine checks passed"
    exit 0
fi
echo "$failures cross-machine check(s) failed" >&2
exit 1
