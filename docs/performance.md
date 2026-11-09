# Cost and measurement

What the cell plane costs on the wire, and how to reproduce the rendering and
latency measurements.

## What it costs

Measured by `tools/bench.sh` at 24×80, against the raw PTY byte count a
byte-forwarding tier would have carried. Every run asserts the receiver ended
up holding the sender's exact screen and fails if it did not.

| Workload | PTY bytes | Wire bytes | Reduction |
|---|---|---|---|
| Idle, 5 s | 0 | one screen, then nothing | — |
| 200 keystrokes | 200 | 184 | 1.1× |
| `vim`, 60 page-downs | 39,578 | 790 | **50×** |
| 1 MB of base64 | 1,062,193 | 12,047 | **88×** |
| 268 KB of text | 268,386 | 2,009 | **134×** |

Typing is the one case with little to gain — 200 keystrokes really are about
200 bytes of information. Its problem is latency, not size, which is what the
predictor is for.

### Reproducing rendering and latency measurements

The optimizations target sparse editing: cached terminal conversion, unchanged
row comparisons, and partial cell runs. The cache adds one grid per terminal
and a snapshot copy; dense redraw workloads can regress. Measure your own
workload before treating these as a general throughput improvement.
The benchmark tools are development diagnostics, not release acceptance gates;
the optional SSH mode requires separate qualification in the target environment.

```sh
make all build/libkilix-mux.so build/bench-grid.so
make bench-native
python3 tools/render_bench.py --output build/render-bench.json
# Use a separately built checkout on an SSH host:
python3 tools/render_bench.py --remote SSH_HOST --remote-root /path/to/checkout \
  --output build/render-ssh.json
```

The native benchmark records parsing, encoding, decoding, ANSI generation,
terminal replay, payload bytes, and complete viewport equality. It uses a
synthetic clock and immediate acknowledgements; its frame count is **not FPS**.
Warmups are excluded, cases are shuffled reproducibly, and raw samples are saved.
`--receiver-library /path/to/older/libkilix-mux.so` checks compatibility with an
older decoder and renderer.

The render benchmark runs the actual client in a correctly sized PTY, with
prediction disabled and no second observer. It compares every cell, colour,
attribute, and cursor with a deterministic expected screen. Read completion is
timestamped before validation; parsing still adds service time between reads
and is reported separately. The oracle uses the same terminal implementation,
so this is not independent terminal conformance or physical display latency.
Recovery is timed only after an independent control channel confirms that the
host has finished the burst. Only benchmark-owned processes are suspended or
terminated.

For an interleaved comparison, add `--baseline-attach /path/to/old/kmx-attach`
and `--baseline-server /path/to/old/kmx-serve` locally, or
`--baseline-remote-root /path/to/old/checkout` remotely. Both variants use the
same validation library; `--library` can pin that independently. Each defaults
to three runs of 100 samples at four sizes, with ten warmups. Compare the spread
across runs as well as percentiles; failures exit nonzero and remain in JSON.
Delay/rate shaping remains available in `tools/coding_bench.py`; it models
stream scheduling, not packet loss. Generated burst frames may be coalesced.

The PTY owner answers OSC 10/11 default-colour queries using its logical
palette (light foreground, black background) and advertises truecolour. This
lets terminal applications choose shaded input areas before a client attaches.
It does not synchronize a client's custom theme to the host.
