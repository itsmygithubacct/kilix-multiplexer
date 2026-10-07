# Coding sessions

The coding-session path carries a terminal viewport and its input state over
the existing ordered KMX stream. It can run an interactive coding CLI
under an owned PTY, or observe a broker-owned pane. The application remains on
the server when a client disconnects.

```sh
make all test-coding
build/kmx-serve --socket ./coding.sock -- codex
build/kmx-attach --socket ./coding.sock --no-predict --no-audio
```

Choose the application's own model and account settings outside this checkout.
Use the existing SSH tunnel or authenticated TLS options for remote access.
`--no-predict` is useful when measuring confirmed remote responses.

## State and delivery

Cells describe the current viewport. Each update replaces complete selected
rows and includes the cursor position and visibility. The sender retains a
bounded acknowledged baseline and includes rows changed by unacknowledged
intermediate screens, including rows that subsequently revert. This prevents
an A→B→A change from leaving B on the receiver after a delayed ACK. These
updates retain the existing cell format and work with older decoders.

One of the eight retained snapshots anchors ACK progress when the round trip
exceeds the recent-history window. An ACK for an evicted sequence can advance
to an earlier retained snapshot that it cumulatively covers, without clearing
later repair obligations. Adaptive pacing retries unchanged screens after two
measured round trips (one second before the first sample), while changed
screens use the usual repaint interval.

Delivery relies on an ordered stream. The correction does not make cell
messages safe to apply out of order. Superseded viewport updates can be skipped
by the synchronizer; user input cannot. Reconnect starts with fresh viewport
state. The current protocol does not retain the output that has scrolled off
that viewport.

The PTY owner answers libvterm-supported terminal queries locally through
`kmx_term_set_output_callback`. A passive broker observer leaves this callback
unset, avoiding duplicate replies to the application. The terminal model
preserves primary and alternate screen contents. The renderer draws the
current visible viewport and preserves cursor visibility and underline styles.

The server observes synchronized-output boundaries (`CSI ? 2026 h` / `l`)
before publishing cells and graphics. This keeps a repaint split across PTY
reads from consuming several paced updates. A 200 ms maximum hold prevents a
missing end marker from freezing the viewport; input, terminal replies, and
other panes continue during the hold. This is server-side scheduling and adds
no wire flag. The synthetic benchmark uses the same repaint boundaries.

Controller input uses a bounded FIFO. When the child stops reading, the server
retains the current complete INPUT message and pauses reads from that client
until it can accept the message. Other clients can continue receiving output.
Terminal replies share the FIFO and have reserved headroom beyond the
controller limit. Exhausting the hard bound is an explicit session error.

The attach client also queues complete frames and resumes partial writes after
socket backpressure, for both plain and TLS connections. It pauses stdin at a
soft bound while continuing to receive state and queue ACKs. On disconnect it
reports delivery uncertainty and discards pending transport bytes and unread
interactive terminal input. It never replays old input automatically. Exactly
once delivery across reconnect requires a future input acknowledgement and
deduplication protocol.

## Terminal-mode extension

`TERMINAL_MODES` (message type 14) is an optional server-to-client full-state
message. It follows the layout for a greeted client and is repeated when the
focused pane or its modes change. Its payload is exactly eight bytes:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | Version, currently 1 |
| 1 | 1 | Focused pane index |
| 2 | 2 | Reserved, both zero |
| 4 | 4 | Big-endian mode flags |

Flags are application cursor keys (`0x01`), bracketed paste (`0x02`), focus
reports (`0x04`), click/drag/all-motion mouse tracking (`0x08`/`0x10`/`0x20`),
and SGR mouse encoding (`0x40`). At most one mouse-tracking bit can be set.
Unknown flags, versions, nonzero reserved bytes, invalid lengths, and a pane
that disagrees with the current focused layout are rejected.

The interactive text controller applies this state to its local terminal.
Viewers and dump clients do not enable input reports; pixel clients keep their
existing input protocol. Mouse tracking is enabled only for a single text
pane because multi-pane mouse coordinates still need translation. Keyboard,
paste, and focus modes follow the focused pane in a multi-pane layout. Input
modes are cleared on reconnect and detach, and the cursor is shown on exit.

Older clients ignore the unknown message type. A new client connected to an
older server retains its legacy input behavior. This extension does not
negotiate enhanced keyboard protocols or application keypad mode.

## Repeatable measurements

```sh
make test                 # native terminal, mode, and delayed-ACK regressions
make test-coding           # owned PTYs/sockets, actual client, input integrity
python3 tools/coding_bench.py --samples 50 --output /tmp/coding-lan.json
python3 tools/coding_bench.py --samples 50 --delay-ms 250 --rate-bytes 32000 \
    --output /tmp/coding-shaped.json
python3 tools/coding_bench.py --remote SSH_HOST --remote-root /path/to/checkout \
    --rows 57 --cols 212 --samples 50 --output /tmp/coding-remote.json
```

The remote checkout must have `build/kmx-serve`. The harness creates its own
server, PTY workload, and temporary socket; it does not use a person's live
coding session. It uses the local native decoder through `build/libkilix-mux.so`.
For another build directory, pass `--server` and `--library` explicitly.
The process harnesses require Linux and Python with pidfd support for cleanup.

The synthetic editor writes its confirmation marker after the rest of each
repaint. Latency runs from input enqueue to that marker in the decoded remote
grid. Every confirmed final grid is checked, including recovery after output
continues while detached. Reported bytes are actual framed KMX bytes and exclude
SSH, TLS, TCP, and lower-layer overhead. Delay and rate settings schedule
application-frame delivery; they do not model TCP packet loss or qualify a
network link. Local rendering bytes and predicted echo are separate measures.

`test-coding` checks terminal replies without an attached client, exact 1 MiB
paste delivery under PTY backpressure, a concurrent responsive viewer, replies
during that paste, actual attach input modes, and exact 8 MiB client transfers
under plain/TLS backpressure. The TLS tests use an owned temporary certificate
and require the `openssl` command. Test cleanup targets only owned processes.
Native delayed-ACK tests include deterministic reversions,
scrolling, resizes, reconnect resets, and randomized schedules.

## Further protocol work

A complete coding-session experience also needs independently addressable
history. Add durable transcript/scrollback storage with sequence ranges,
explicit retention bounds, and on-demand retrieval; keep that traffic separate
from current viewport catch-up. A viewport diff cannot reconstruct output that
was never retained.

Further cycles should cover enhanced keyboard and terminal capability
negotiation, multi-pane mouse coordinates, and input ownership across multiple
controllers. Measure real CLI workloads as well as
the deterministic fixture, including Unicode edits, long tool output, resize,
and interruption under constrained links. Passing the synthetic benchmark does
not establish compatibility with every coding CLI version.
