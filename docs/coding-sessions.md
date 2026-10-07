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
soft bound while continuing to receive state and queue ACKs. In legacy mode, disconnect
reports delivery uncertainty and discards pending transport bytes and unread
interactive terminal input. Legacy input is never replayed automatically.

## Acknowledged input and reconnect

For a single owned text PTY, both updated endpoints support an opt-in input
ledger:

```sh
build/kmx-serve --socket ./coding.sock -- codex
build/kmx-attach --socket ./coding.sock --reliable-input --no-predict --no-audio
```

`--reliable-input` waits for a successful handshake before sending any input.
An older server is rejected after a five-second selection deadline; there is
no automatic downgrade. Viewers, pixel input, broker panes, and multi-pane
sessions do not support this mode. Legacy clients retain their existing wire
format when no reliable controller owns the session.

The client creates a random 128-bit identity and assigns consecutive input
sequence numbers starting at one. The server grants one exclusive controller
lease and creates a random 128-bit ledger epoch. An acknowledgement means
that the complete input message is owned by the server's bounded FIFO. It
does **not** mean that the child has read, parsed, or executed those bytes.
The server advances its accepted counter before attempting any partial PTY
write. An already accepted sequence is acknowledged without appending its
bytes again. A gap or zero sequence closes the connection.

The client retains unacknowledged messages in memory: at most 256 KiB plus one
32 KiB message, and at most 4,096 messages. Stdin pauses when either bound is
reached, while viewport traffic, acknowledgements, timers, and termination
signals remain serviceable. A peer cannot acknowledge merely allocated
messages that have never been offered to the transport. For TLS, a complete
frame included in a write attempt may have reached the server even if that
attempt returns WANT or subsequently fails, so acknowledgement validation
conservatively includes such frames. Ctrl-] is part of the stdin byte stream
and can wait behind input backpressure; SIGTERM and `--seconds` still stop
the client.

On automatic reconnect, the client presents its epoch, identity, and last
acknowledged counter. The server returns its current accepted counter. The
client removes confirmed entries and resends the remaining entries with their
original sequences; partial transport frames are reconstructed in full. The
server keeps the ledger for 60 seconds after the owner disconnects. A valid
resume fences the old transport, including its retained input frame. Other
controllers cannot inject input, change focus, or resize the PTY during the
lease or its disconnect grace. Viewers may continue receiving output.

Server restart, expired retention, an unknown identity, an inconsistent
counter, or a different epoch stops recovery without replay. Every newly
allocated ledger gets a fresh epoch, including when a client identity is
reused after close or expiry. Old frames therefore cannot become valid in a
new ledger. This is bounded deduplication within a running server, not durable
exactly-once application execution. The client journal is not persisted across
client process exit, and server acceptance does not survive a server crash.

When detaching with a fully acknowledged journal, the client attempts an
explicit lease release for up to 100 ms. If it does not reach the server, the
60-second grace still applies. Leaving with unacknowledged input reports the
remaining byte count and exits unsuccessfully. A protocol error discards
pending transport frames instead of flushing them during cleanup.

### Input wire extension

Payload helpers are in `kilix_mux_input.h`; the transport-independent ownership
and deduplication state machine is in `kilix_mux_input_session.h`. All integer
fields below are unsigned and big endian. Versions, reserved bytes, sizes,
identities, and counter bounds are checked before state is changed.

| Type | Direction | Payload |
| --- | --- | --- |
| 15 `INPUT_OPEN` | Client → server | 44 bytes: version=1, three zero bytes, epoch[16], identity[16], last ACK[8] |
| 16 `INPUT_STATE` | Server → client | 48 bytes: version=1, status, two zero bytes, epoch[16], identity[16], accepted[8], disconnect grace in ms[4] |
| 17 `INPUT_DATA` | Client → server | Version=1, pane, two zero bytes, sequence[8], then 1–32,768 input bytes |
| 18 `INPUT_ACK` | Server → client | 12 bytes: version=1, three zero bytes, accepted[8] |
| 19 `INPUT_CLOSE` | Client → server | Same payload as ACK; release requires the active owner's exact accepted counter |

A zero epoch in OPEN requests a new ledger and requires last ACK zero. The
identity must be nonzero. STATE always contains a nonzero epoch and echoes the
request identity; error states expose no other owner's accepted counter.
Status values are READY=0, BUSY=1, EXPIRED=2, EPOCH=3, LIMIT=4, and DENIED=5.
LIMIT is reserved for an admission policy refusal; ordinary FIFO pressure
pauses input instead. DATA currently requires pane zero. Duplicate frames are
identified by sequence within the ledger; they never replace prior bytes.
TLS or the existing authenticated SSH/Unix transport remains responsible for
peer identity and confidentiality; the ledger identity is not a substitute.

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
make test-input           # input codecs, lease state, fault-injected client writes
make test-coding           # owned PTYs/sockets, actual client, input integrity
python3 tools/coding_bench.py --samples 50 --output /tmp/coding-lan.json
python3 tools/coding_bench.py --samples 50 --delay-ms 250 --rate-bytes 32000 \
    --output /tmp/coding-shaped.json
python3 tools/coding_bench.py --remote SSH_HOST --remote-root /path/to/checkout \
    --rows 57 --cols 212 --samples 50 --output /tmp/coding-remote.json
python3 tools/coding_bench.py --reliable-input --remote SSH_HOST \
    --remote-root /path/to/checkout --rows 57 --cols 212 --samples 50 \
    --output /tmp/coding-reliable-remote.json
```

The remote checkout must have `build/kmx-serve`. The harness creates its own
server, PTY workload, and temporary socket; it does not use a person's live
coding session. It uses the local native decoder through `build/libkilix-mux.so`.
For another build directory, pass `--server` and `--library` explicitly.
The process harnesses require Linux and Python with pidfd support for cleanup.

The benchmark defaults to legacy input. With `--reliable-input`, its synthetic
controller negotiates the ledger, validates cumulative acknowledgements, and
retains its identity, epoch, and outstanding input across reconnect. It checks
both recovered viewport contents and a subsequent acknowledged edit. JSON
records `input_protocol` and the final `input_accepted` counter. This exercises
the real server with a Python protocol client; the actual `kmx-attach` client
is covered separately by `test-coding`, including exact 70,000-byte delivery
to the real server, graceful detach, and acquisition by a new controller.

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

`test-input` requires no listener and exercises the actual client journal with
partial-write and TLS-WANT injection. `test-coding` also runs the input-resume
socket harness. To check the real retention deadline, run
`python3 tests/test_input_resume.py --expiry`; this adds a 60-second wait.
Restricted environments that deny socket listeners cannot run the transport
suite; native state tests do not establish end-to-end transport compatibility.

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
