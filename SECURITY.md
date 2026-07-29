# Security review

Status: reviewed 2026-07-28 against the tree at that date
Scope: `kilix-multiplexer` only. `kitty-pty-broker` has its own posture.

This is a program that puts a shell on a socket. The review below is written
to be argued with: each claim says what enforces it and where, so a reader can
check rather than trust.

## What an attacker is assumed to be

Two different peers, with different powers:

| Peer | Reaches | Assumed to be |
|---|---|---|
| A local process | the Unix socket | Possibly hostile, but running as some user on this machine |
| A network peer | a TCP port | Anyone who can route to it |

The design assumes the second is reached **through an SSH tunnel**, and that
SSH is what establishes who they are. That assumption is load-bearing and is
the reason for the loopback default; where it does not hold, see
[Known gaps](#known-gaps).

## What is enforced, and where

### Reachability

TCP binds loopback unless `--lan` is given (`src/endpoint.c`,
`kmx_endpoint_listen`). A non-loopback bind is **refused**, not narrowed: a
caller that asked for a public address and silently got loopback would believe
it was reachable when it was not. With `--lan` the server says on stderr that
anyone who can connect gets the session.

Asserted by `tests/integration.sh`, "a non-loopback bind is refused unless
asked for".

### Who may connect

On a Unix socket, the peer's credentials are checked with `SO_PEERCRED` and a
peer whose uid differs from the server's is closed immediately
(`tools/kmx_serve.c`, `peer_is_owner`). The socket itself is created `0600`.

Over TCP there is no equivalent, so a **token** is required instead. It is
mandatory whenever the bound address is genuinely reachable, cannot be turned
off, and is minted from `/dev/urandom` (128 bits, hex) unless one is supplied.
The requirement follows the *address*, not the `--lan` flag: `--lan` on a
loopback address reaches nobody new, and demanding a token there would be a
requirement the operator never asked for.

Comparison is constant-time (`token_matches`): a check that returns on the
first wrong character tells an attacker how much of it was right. Refusal is
silent — a peer that cannot present the token learns only that the connection
closed — and **nothing at all is accepted before a valid greeting**.

Asserted by `tests/integration.sh`: a token is minted, and attaching with
none, with a wrong one, and with the right one give refusal, refusal, and a
session. Verified across two machines over a real LAN with no tunnel.

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
| Decompressed size | `KMX_MAX_CELLS * 16` | `src/sync.c`, `kmx_decompress` |
| Pane count and geometry | `KMX_MAX_PANES`, must fit the screen | `src/layout.c` |
| Image payload | `KMX_MESSAGE_MAX`; cache bounded in entries and bytes | `src/graphics.c` |
| Motion frame | 8192 per axis, 64 MiB total | `src/motion.c` |
| Audio block | `KMX_AUDIO_BLOCK_MAX`, 1 MiB; rate and channels range-checked | `src/audio.c` |
| Clients | `KMX_MAX_CLIENTS`, 8 | `tools/kmx_serve.c` |
| Per-client backlog | `KMX_CLIENT_QUEUE_LIMIT`, 4 MiB, then disconnect | `tools/kmx_serve.c` |

### What a peer can make the server do

Nothing that blocks it. Client sockets are non-blocking with a bounded
backlog, so a peer that stops reading is disconnected rather than allowed to
stall the panes or the other clients — asserted by an integration check that
connects and never reads while a pane floods.

### Parsers

Every decoder a peer can reach is fuzzed under ASan and UBSan: cells, layout,
image, framer, decompressor, motion, audio, and the receiver end to end
(`tests/fuzz_decoders.c`, `make fuzz`). Compositing a decoded layout is fuzzed
too, because that is what the client does with it next.

The suite additionally asserts that **every proper prefix** of a valid message
is rejected and that trailing bytes are an error rather than ignored.

## Known gaps

Stated plainly rather than left to be discovered.

1. **No transport encryption.** Content crosses the network in the clear
   without a tunnel. Over loopback that is fine; over a reachable address it
   is not, and the server says so when it binds one. A token establishes *who*
   may attach; it does nothing about *who may watch*. Kilix's own tiers add
   TLS for this (`config/stream.py`) and this project has not; until it does,
   a reachable bind is only appropriate on a segment you would already trust
   with the terminal's contents.
3. **No rate limit on connection attempts.** Eight client slots fill on a
   first-come basis, so a local process can occupy them.
4. **The pixel pane runs a shell command** given on the server's own command
   line. That is the operator's own input, not a peer's, but it is worth
   naming: a peer cannot start a pane, and there is no message that does.
5. **The audio and pixel sources are shell commands** with the same property.
6. **Independent review has not been completed.** The tests and defect history
   in this document are evidence about specific behavior, not a substitute for
   an independent security review.

## Things deliberately not done

- **No new cryptography.** When transport security lands it will be TLS or an
  SSH tunnel, not a bespoke handshake.
- **No persistence.** The multiplexer writes nothing to disk; session content
  lives only in memory and in whatever the broker already records.
- **Private displays for pixel panes.** A pane's X client runs on a display
  created for the session, so it cannot draw on, or read from, a display
  someone is using.

## Before any public release

- [x] Token authentication for reachable binds — done 2026-07-28
- [ ] TLS, so a reachable bind is confidential as well as authenticated
- [ ] Independent review of the LAN path
- [ ] Extended fuzzing run against a seeded corpus, not just the default 30s
