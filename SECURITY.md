# Security review

Status: reviewed 2026-07-28 against the tree at that date
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

A network peer must present a token, and the recommended route is still an SSH
tunnel to a loopback port - the token establishes *who may attach*, the tunnel
establishes *who may watch*. Both matter, and only the first is implemented;
see [Known gaps](#known-gaps).

## What is enforced, and where

### Reachability

TCP binds loopback unless `--lan` is given (`src/endpoint.c`,
`kmx_endpoint_listen`). A non-loopback bind is **refused**, not narrowed: a
caller that asked for a public address and silently got loopback would believe
it was reachable when it was not. When the bound address is reachable the
server prints the attach line including the token, and says plainly that the
session crosses the network in the clear.

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
rather than falling back to something unencrypted. Verified between two
machines over a real LAN with no tunnel.

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

### How often a peer may try

Accepts are drawn from a bucket of `KMX_ACCEPT_BURST` (16) refilled at
`KMX_ACCEPT_PER_SECOND` (8). Beyond that, connections are closed without a
word — distinguishing a flood from a mistake is not this layer's job, and
answering costs more than ignoring. The count of refusals is reported at
shutdown so the behaviour is visible rather than silent.

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
5. **Not reviewed by anyone else.** This is a self-review by the author of the
   code, which is worth less than an independent one.

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
- [x] TLS with fingerprint pinning — done 2026-07-28
- [x] A reachable bind encrypts by default; `--no-tls` is explicit (2026-07-29)
- [x] Accept rate limiting (2026-07-29)
- [ ] Independent review of the LAN path
- [x] Seeded corpus — `make fuzz` builds it from real messages (2026-07-29)
- [x] A long soak — 25 minutes, 2 workers, **found a real bug** (2026-07-29):
      the motion decoder validated its dimensions after narrowing them to
      `int`, so a width of 0x100000001 passed as 1 and then sized an
      allocation from the full value. Fixed, pinned by a regression test, and
      the other decoders audited for the same shape (they bound the 64-bit
      value first, which is the correct order)
- [ ] A longer soak still, hours rather than minutes, before publication
