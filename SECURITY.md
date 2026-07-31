# Security review

Status: self-reviewed through 2026-07-29; no independent review
Scope: `kilix-multiplexer` only. The broker's read-only observers are argued
separately in `kitty-pty-broker/SECURITY.md`, which matters here because the
multiplexer attaches as one: a bug in this program should not become control
of the shell.

This is a program that puts a shell on a socket. The review below is written
to be argued with: each claim says what enforces it and where, so a reader can
check rather than trust.

## What an attacker is assumed to be

Two different peers, with different powers:

| Peer | Reaches | Assumed to be |
|---|---|---|
| A local process | the Unix socket | Possibly hostile, but running as some user on this machine |
| A network peer | a TCP port | Anyone who can route to it |

A network peer must present a token. A directly reachable listener also uses
TLS pinned to the server's printed fingerprint: the token establishes *who may
attach* and the pin establishes *which server may be watched*. An SSH tunnel
to a loopback port is still the lower-exposure route because it removes the
reachable listener entirely.

## What is enforced, and where

### Reachability

TCP binds loopback unless `--lan` is given (`src/endpoint.c`,
`kmx_endpoint_listen`). A non-loopback bind is **refused**, not narrowed: a
caller that asked for a public address and silently got loopback would believe
it was reachable when it was not. When the bound address is reachable the
server enables TLS by default and prints the attach line including both the
token and certificate fingerprint. If the operator explicitly supplies
`--no-tls`, it says plainly that the session crosses the network in the clear.

Asserted by `tests/integration.sh`, "a non-loopback bind is refused unless
asked for".

### Who may connect

On a Unix socket, the peer's credentials are checked with `SO_PEERCRED` and a
peer whose uid differs from the server's is closed immediately
(`tools/kmx_serve.c`, `peer_is_owner`). The socket itself is created `0600`.

Over TCP there is no equivalent, so a **token** is always required instead,
including on loopback. A loopback TCP listener still lacks `SO_PEERCRED`, so
without a token another local user could attach. The token cannot be turned
off and is minted from `/dev/urandom` (128 bits, hex) unless one is supplied.
The requirement follows the transport, not the `--lan` flag.

Comparison is constant-time (`token_matches`): a check that returns on the
first wrong character tells an attacker how much of it was right. Refusal is
silent — a peer that cannot present the token learns only that the connection
closed — and **nothing at all is accepted before a valid greeting**.

Asserted by `tests/integration.sh`: a token is minted, and attaching with
none, with a wrong one, and with the right one give refusal, refusal, and a
session. `tests/lan.sh` repeats the refusal and recovery across two machines
over a real LAN with no tunnel.

### What a peer can read

`--tls` encrypts the connection. There is no certificate authority, and there
should not be one for a personal session between two machines that already
know each other: the server mints a key and a self-signed certificate at
startup, holds both **in memory only**, and prints the certificate's SHA-256
fingerprint. The client is given that fingerprint and refuses anything else.

The client will not run without one. A client that would accept any
certificate has no more assurance than no TLS at all — and a comforting one,
which is worse. The comparison is constant-time, for the same reason the
token's is.

Asserted by `tests/integration.sh`: the right fingerprint attaches, a wrong
one is refused, and a plaintext client against a TLS server gets nothing
rather than falling back to something unencrypted. `tests/lan.sh` repeats all
three cases between two machines over a real LAN with no tunnel.

### What a peer may do

Roles are declared in `HELLO` and enforced by the server, not by the client.
A viewer's `INPUT` is dropped where it arrives (`tools/kmx_serve.c`,
`item->control` check), and its `RESIZE` and `FOCUS` are ignored, so a viewer
cannot move the session out from under a controller.

The integration check has the viewer **send input anyway** and asserts it never
reaches the pane. A client that merely chose not to send would prove nothing.

### What a peer can make the server allocate

Every length that arrives from a peer is bounded before it is believed:

| Input | Bound | Where |
|---|---|---|
| Frame length | `KMX_MESSAGE_MAX`, 8 MiB | `src/frame.c` |
| Pending unparsed bytes | 2 × `KMX_MESSAGE_MAX` | `src/frame.c`, `kmx_framer_push` |
| Grid dimensions | `KMX_MAX_DIMENSION`, `KMX_MAX_CELLS` | `include/kilix_mux.h`, `src/grid.c` |
| Decompressed size | per plane, passed in: `KMX_CELLS_WIRE_MAX` for cells, 64 MiB + 64 KiB for motion, `KMX_AUDIO_BLOCK_MAX` + 64 for audio | `src/sync.c`, `kmx_decompress` |
| Pane count and geometry | `KMX_MAX_PANES`, must fit the screen | `src/layout.c` |
| Image payload | `KMX_MESSAGE_MAX`; cache bounded in entries and bytes | `src/graphics.c` |
| Motion frame | 8192 per axis, 64 MiB total | `src/motion.c` |
| Audio block | `KMX_AUDIO_BLOCK_MAX`, 1 MiB; rate and channels range-checked | `src/audio.c` |
| Presenter tap frame | 8192 per axis, 64 MiB total; exact session id | `tools/kmx_tap.c` |
| Clients | `KMX_MAX_CLIENTS`, 8 | `tools/kmx_serve.c` |
| Per-client backlog | `KMX_CLIENT_QUEUE_LIMIT`, 4 MiB, then disconnect — except the motion and audio planes, which drop | `tools/kmx_serve.c` |
| Per-pane typed-ahead input | `KMX_PANE_INPUT_LIMIT`, 256 KiB, then dropped | `tools/kmx_serve.c` |
| Time to finish connecting | `KMX_SETTLE_MS`, 10 s for handshake and HELLO | `tools/kmx_serve.c` |

One correction to the row above. `kmx_decompress` originally took no bound and
applied a single cell-plane figure to every caller, which was both too small
for the motion plane and far too large for audio. It is now a parameter,
because a cell message and a video frame have no business sharing a limit.

### How often a peer may try

Accepts are drawn from a bucket of `KMX_ACCEPT_BURST` (16) refilled at
`KMX_ACCEPT_PER_SECOND` (8). Beyond that, connections are closed without a
word — distinguishing a flood from a mistake is not this layer's job, and
answering costs more than ignoring. The count of refusals is reported at
shutdown so the behaviour is visible rather than silent.

### What a peer can make the server do

Nothing that blocks it — but that claim was wrong twice, and both are worth
recording rather than quietly fixed.

Client sockets are non-blocking with a bounded backlog, so a peer that stops
reading is disconnected rather than allowed to stall the panes or the other
clients — asserted by an integration check that connects and never reads while
a pane floods. That part held.

**Writes to the pane did not.** Input arriving from a client was written to the
pty master with a blocking write, so a program that stopped reading its own
input could stop the server: not just its own pane, but every pane, every other
client, and the shutdown path. Reaching it needs a pane in raw mode with echo
off — in canonical mode the tty echoes input back and the server drains it, so
the buffer never fills, which is why it survived every earlier test. Measured
against the version before the fix, the server stopped serving after 0.1 MB and
had to be killed. Input is now queued per pane, bounded, and drained on POLLOUT
like everything else; `tests/backpressure.sh` fails against the old binary and
passes against the new one.

**And over TLS none of that worked at all.** The outbound queue is compacted
and reallocated as it drains, so the pointer handed to `SSL_write` could move
between a `WANT_WRITE` and its retry — which OpenSSL treats as a caller bug
(`SSL_R_BAD_WRITE_RETRY`) and answers by killing the session. Measured: a TLS
client that stopped reading died at about 320 kB of backlog against a 4 MiB
limit, with no drop counted, while the identical plain-TCP client survived and
dropped 325,100 audio blocks as designed. So the fix below existed and TLS
routed around it. `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` is what the mode is
for; both planes now behave the same encrypted or not.

Two smaller ones in the same layer: the fatal arms of `kmx_tls_read` and
`kmx_tls_write` returned failure without setting `errno`, and since this layer
plants `EAGAIN` on every ordinary would-block, a caller reading a stale `errno`
after a hard failure reliably saw `EAGAIN` and waited forever on a dead session.
And `kmx_tls_wants_write` — which exists precisely so a poll loop can learn that
a *read* needs the socket writable — had no callers at all.

**The client could be hung by a server.** `kmx-attach` polls its socket but
never set `O_NONBLOCK`. On the plain path that is harmless; under TLS `SSL_read`
cannot return until a whole record has arrived, so a server that sends three
bytes of a five-byte record header stops the client's loop dead — `--seconds`
ignored, Ctrl-] ignored, SIGTERM ignored, because `signal()` installs
`SA_RESTART` and the interrupted read simply restarted. Measured: SIGKILL at 12
seconds before, exit 0 at exactly `--seconds 3` after. The handshake is still
blocking, under a timeout, because setting the flag before `SSL_connect` turns
every TLS attach into an instant silent failure — which is what the first
version of this fix did.

**Two more places counted sockets instead of served clients.** The graphics
retention guard treated a freshly accepted socket as "someone is attached" and
discarded the queued images the retention exists to keep — so the documented
behaviour, that a client arriving just after a pane drew something still
receives it, was unreachable, and a rotation of silent peers made it permanent.
The geometry loop let a peer that had neither authenticated nor finished a TLS
handshake vote on the session's size: measured, one silent peer held a
legitimate client's 40x100 at 24x80 for ten seconds, and a staggered handful
held it indefinitely. Both now use one `client_is_served` predicate shared with
the send loop, so the three cannot drift apart again.

**And the listener was blocking**, so an accept-ready connection that
evaporated before `accept4` would have stalled the whole server —
documented in accept(2), not reproducible here, fixed by the missing
`SOCK_NONBLOCK`.

**And the backlog limit punished the wrong plane.** A client that overran the
4 MiB backlog was disconnected, which is right for the cell plane — a client
that far behind is better replaced than buffered — and wrong for motion and
audio, whose whole contract is that they may drop. An incompressible 1080p
frame is 6,220,814 bytes on the wire (measured, not estimated), so it overran
the backlog, dropped the client mid-stream, and dropped it again on the next
such frame after it reconnected. The one plane documented as droppable was the
one that could kill the connection. Those two planes now discard and count;
the count is printed at shutdown so a session dropping everything is visible
rather than silent. Raising the decompression bound to admit a 64 MiB frame is
what exposed this: it made the decoder agree that such a frame is legitimate
while nothing downstream could carry one.

**The TLS handshake did not either.** It ran to completion inside the accept
path on a blocking socket, so a peer that connected and then went quiet held
the whole loop. A receive timeout was not enough on its own: it is per-recv, so
a peer sending a byte every four seconds resets it forever. The handshake is
now driven from the poll loop a step at a time, and a connection that has not
finished handshaking *and* sent HELLO within `KMX_SETTLE_MS` is released —
swept over every slot, not only the readable ones, because a peer that has gone
quiet generates no events. Asserted by a test that fills all eight slots with
silent peers and checks a real client gets in afterwards.

**Live pane observation and input are different descriptors.** The broker-v2
observer runs in a child process and can only write pane output to a
non-blocking pipe. It never receives client input and never claims the broker's
control slot. If live control is enabled, input goes to a separately supplied
helper through its own non-blocking, bounded pipe; without that helper the pane
is view-only. The presenter tap is an owner-only `0600` Unix socket, checks
`SO_PEERCRED`, requires the exact broker session id in every frame, and retains
only one complete newest frame.

### Parsers

Every decoder a peer can reach is fuzzed under ASan and UBSan: cells, layout,
image, framer, decompressor, motion, audio, and the receiver end to end
(`tests/fuzz_decoders.c`, `make fuzz`). Compositing a decoded layout is fuzzed
too, because that is what the client does with it next.

The suite additionally asserts that **every proper prefix** of a valid message
is rejected and that trailing bytes are an error rather than ignored.

Fuzzing found one bug that short runs did not: the motion decoder validated
dimensions *after* narrowing them to `int`, so a declared width of 0x100000001
truncated to 1, passed the check, and then asked for twenty petabytes. It also
missed a second bug of the same shape for a while because the fuzz targets
built a fresh sink per input, which made the delta path unreachable — the sinks
are now persistent, and the delta path is reached.

`clang --analyze` runs clean across `src/` and `tools/`. It found three things
the compiler did not: an uninitialised `count` read in the motion decoder's
loop condition, a `memcpy` from a null pointer with zero length in the image
cache, and a fingerprint comparison that would have read uninitialised bytes if
`X509_digest` ever returned a length other than 32.

## How to check the claims above

Reading is the weakest of these and is listed last on purpose.

| Check | What it would catch |
|---|---|
| `make test` | the library's own invariants |
| `make sanitize` | memory errors under ASan and UBSan |
| `FUZZ_SECONDS=600 make fuzz` | decoder bugs; found two that short runs did not |
| `tests/backpressure.sh` | a pane that stops reading stopping the server |
| `tests/churn.sh` | connections abandoned at eight points of the handshake, under ASan |
| `tests/integration.sh` | the planes end to end, tokens, TLS pinning, a hostile link |
| `tests/network.sh REMOTE` | two real machines, both directions through an SSH tunnel |
| `tests/lan.sh REMOTE` | direct routed TLS both ways, refusal cases, tapped RGB and PCM |
| `clang --analyze src/*.c tools/*.c` | found three things the compiler did not |

`tests/churn.sh` is the one worth explaining, because both of the mistakes it
was written around are easy to repeat. Its first version left 2,968 of 3,000
connections refused by the accept rate limiter, so it exercised almost nothing
while appearing to pass — it now paces itself to the server's allowance and
fails outright if the server reports a single refusal. And it builds a
deliberately leaking program first to confirm LeakSanitizer reports in this
environment, because otherwise a clean run is not evidence of anything. 1,600
connections across eight abandonment points currently produce no findings.

That is a real result and a narrow one: it says the state machine does not
corrupt memory under churn. It says nothing about whether it does the right
thing, which is what a reader is for.

## Known gaps

Stated plainly rather than left to be discovered.

1. **A reachable bind is only as private as the segment, if TLS is refused.**
   Encryption is now the default for a reachable address; `--no-tls` turns it
   off and says so, which is reasonable inside a tunnel and not otherwise.
2. **The accept rate limit is a brake, not a defence.** A bucket of 16 with 8
   per second stops a loop turning a wrong token guess into an unbounded one,
   and bounds what a discovered port costs. It does not stop a determined
   local process, which shares the user's privileges anyway.
3. **The pixel pane runs a shell command** given on the server's own command
   line. That is the operator's own input, not a peer's, but it is worth
   naming: a peer cannot start a pane, and there is no message that does.
4. **The audio and pixel sources are shell commands** with the same property.
5. **A peer can still cost the server time, just not stop it.** Every blocking
   path above now has a bound; none of them is zero. A same-uid or
   token-holding peer can occupy a slot for `KMX_SETTLE_MS` and can reconnect.
   The accept bucket is what makes that finite rather than free.
6. **Live input is only as narrow as the supplied helper.** The multiplexer
   never constructs it and cannot prove its policy. Kilix supplies a bounded
   pane-session matcher; a caller using `--input-command` directly is
   responsible for an equivalent scope. For a pixel pane the helper inherits
   the private `DISPLAY`, but what it injects on that display remains the
   helper's policy.
7. **Independent review has not been completed.** The tests and defect history
   in this document are evidence about specific behavior, not a substitute for
   an independent security review.

## Things deliberately not done

- **No bespoke cryptography.** Transport security is OpenSSL TLS with a pinned
  certificate fingerprint, or an SSH tunnel, not a custom handshake.
- **No persistence.** The multiplexer writes nothing to disk; session content
  lives only in memory and in whatever the broker already records.
- **Private displays for pixel panes.** A pane's X client runs on a display
  created for the session, so it cannot draw on, or read from, a display
  someone is using.

## Before any public release

- [x] Token authentication for reachable binds — done 2026-07-28
- [x] TLS with fingerprint pinning — done 2026-07-28
- [x] A reachable bind encrypts by default; `--no-tls` is explicit (2026-07-29)
- [x] Accept rate limiting (2026-07-29)
- [ ] **Independent review of the LAN path — NOT DONE.** Published anyway, on
      the operator's explicit decision (2026-07-29). This is the one item on
      this list that was waived rather than met, and it is the most important
      one, so it is stated here rather than left to be inferred from an
      unticked box.
- [x] Seeded corpus — `make fuzz` builds it from real messages (2026-07-29)
- [x] A long soak — 25 minutes, 2 workers, **found a real bug** (2026-07-29):
      the motion decoder validated its dimensions after narrowing them to
      `int`, so a width of 0x100000001 passed as 1 and then sized an
      allocation from the full value. Fixed, pinned by a regression test, and
      the other decoders audited for the same shape (they bound the 64-bit
      value first, which is the correct order)
- [x] Re-soaked against the fix — 47.9M executions in 15 minutes, clean
- [x] A longer soak, hours rather than minutes (2026-07-30): **566,133,758
      executions over four hours**, against the tree including the slot-eviction
      and persistent-identity changes. No crash, leak, timeout or
      out-of-memory; coverage rose from 852 to 861 and settled. That is roughly
      forty-four times the previous longest run. It found nothing, which is a
      weaker result than the 25-minute soak that found a real bug — but it is
      the first run long enough that finding nothing means something.

## If you are reviewing this

Start with the four claims under "What is enforced, and where" and try to break
them; the section above each fix records what the trigger was, because a
concrete trigger is worth more than a description.

Known-weak spots, so you do not have to rediscover them:

- `KMX_SETTLE_MS` is ten seconds and there are eight client slots, so eight
  peers can keep a session unattachable at low cost. There is no good answer to
  this in the current design.
- The self-signed certificate is regenerated per server start, so a pinned
  fingerprint is per-session and has to be re-copied each time — a usability
  cost that pushes people toward not checking it.
- Every bug found so far in the connection handling has been of one of two
  shapes: a bound checked after a narrowing conversion, or a check that counted
  sockets where it meant clients that were actually being served. Both recurred
  after being fixed once.

A finding with a trigger is worth more than five without. A review that finds
nothing and says so is a real result and the one thing that would let the box
above be ticked.
