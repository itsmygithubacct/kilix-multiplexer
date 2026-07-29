#!/usr/bin/env bash
#
# Regenerate the cell-plane measurements.
#
# Every workload runs a real program under a real PTY.  The comparison figure
# is the raw PTY byte count, which is what a byte-forwarding tier such as tmux
# would have carried for the same work.
#
#   tools/bench.sh [OUTPUT_DIR]
set -u

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
bench="$root/build/kmx-bench"
out=${1:-$root/build/bench}
fixtures="$out/fixtures"

[ -x "$bench" ] || { echo "build kmx-bench first (make)" >&2; exit 2; }
mkdir -p "$fixtures"

# A source-like file: repetitive enough to be realistic, not so repetitive that
# it flatters the compressor.
if [ ! -s "$fixtures/source.txt" ]; then
    python3 - "$fixtures/source.txt" <<'PY'
import random, sys
random.seed(7)
words = ('the quick brown fox jumps over lazy dog function return struct '
         'static void const int char buffer offset result').split()
with open(sys.argv[1], 'w') as handle:
    for line in range(5000):
        count = random.randint(4, 12)
        handle.write('%4d  %s\n' % (line, ' '.join(random.choice(words) for _ in range(count))))
PY
fi
[ -s "$fixtures/flood.log" ] || head -c 1048576 /dev/urandom | base64 | head -c 1048576 > "$fixtures/flood.log"
if [ ! -s "$fixtures/vim.keys" ]; then
    # Sixty page-downs, then quit without saving.
    printf '\004%.0s' $(seq 1 60) > "$fixtures/vim.keys"
    printf '\033:q!\r' >> "$fixtures/vim.keys"
fi

run() {
    local label=$1; shift
    printf '%s\n' "--- $label"
    "$bench" --label "$label" --rows 24 --cols 80 -- "$@" > "$out/$label.json" || {
        echo "  FAILED (did not converge)" >&2
        return 1
    }
    python3 - "$out/$label.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print('  pty %-10d wire %-8d reduction %6.1fx  msgs %-4d cpu %.3fs  converged %s'
      % (d['pty_bytes'], d['wire_bytes'], d['reduction_factor'],
         d['messages'], d['cpu_seconds'], d['converged']))
PY
}

failures=0
run W1-idle    /bin/sh -c 'sleep 5' || failures=$((failures+1))
run W2-typing  /bin/sh -c 'stty -echo; i=0; while [ $i -lt 200 ]; do printf "x"; i=$((i+1)); done' || failures=$((failures+1))
run W3-vim     vim -u NONE -n -s "$fixtures/vim.keys" "$fixtures/source.txt" || failures=$((failures+1))
run W4-flood   /bin/sh -c "cat '$fixtures/flood.log'" || failures=$((failures+1))
run W4b-source /bin/sh -c "cat '$fixtures/source.txt'" || failures=$((failures+1))

echo
if [ "$failures" -eq 0 ]; then
    echo "all workloads converged; results in $out"
    exit 0
fi
echo "$failures workload(s) failed" >&2
exit 1
