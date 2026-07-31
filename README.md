# kilix-multiplexer

Streaming a **graphical Kilix session** to another machine over a slow, lossy,
or high-latency link, without collapsing it into a video stream.

Every remote-desktop system has to *guess* which parts of a screen are text and
which are video, because all it receives is pixels. Kilix does not have to
guess: it is the terminal, so it already knows which rectangles are character
cells, which are Kitty graphics placements, and which panes are live pixel
planes. This project exploits that, carrying a session as typed per-pane
substreams — lossless cell diffs for text, tiles for still images, video only
where there is actually video — rather than one undifferentiated stream.

This release tree contains the portable implementation and its public design
and security notes. Machine-specific integration details live outside it.

**Status: all six planes work end to end** — layout, cells, stills, motion,
audio, and input. A multi-pane session can be served over a Unix socket or TCP,
with TLS and token authentication, attached by several clients at once, with
predictive local echo, client-drawn chrome, and reconnection. Verified between
two machines in both directions. A live Kilix pane can also be observed through
`kitty-pty-broker` v2 while RGB frames arrive through the separate
`kitty-frame-presenter` tap; the observer itself remains read-only.

```sh
make               # library and tools
make test          # unit suite
make sanitize      # under ASan + UBSan
make fuzz          # libFuzzer against the decoder (needs clang)
make check-vendor  # the vendored tree is unmodified upstream
tests/integration.sh   # real processes over a real socket
tests/network.sh HOST  # both directions through an SSH tunnel
tests/lan.sh HOST      # direct routed TLS, both directions and all media
tools/bench.sh         # what the cell plane actually costs
```

## Try it

```sh
# one pane
kmx-serve --socket ./kmx.sock -- bash
# or several, side by side
kmx-serve --socket ./kmx.sock --split horizontal --pane bash --pane 'top -d1'

kmx-attach --socket ./kmx.sock   # Ctrl-] detaches, Ctrl-O changes pane
```

## Local configuration and state

Treat the source checkout as read-only when integrating the multiplexer with a
particular machine or account. Personal wrappers, configuration, logs, and
runtime state belong outside the checkout in a location selected by the
operator. Host aliases, usernames, account/profile choices, local checkout
paths, tokens, and private endpoints do not belong in this repository.

The client renders the screen, forwards typing, follows resizes, and echoes
keystrokes optimistically — underlined until the server confirms them.
`--no-predict` turns that off; `--dump` renders without taking over the
terminal, which is how the integration tests drive it.

Ordinary motion frames are rendered through compressed, chunked Kitty graphics
on the attaching Kilix terminal. Audio is sent to `pacat` or `aplay` when one
is available; `--audio-output COMMAND` selects another raw signed-16-bit PCM
sink and `--no-audio` disables playback.

For a broker-owned Kilix pane, the installed frontend supplies the bookkeeping:

```sh
# PANE_ID comes from: kilix ls --panes
kilix remote serve PANE_ID --socket 127.0.0.1:47800
kilix remote attach --socket 127.0.0.1:47800 \
    --token TOKEN_FROM_SERVER
```

The live server reads PTY output only from a protocol-v2 broker observer.
Typing uses a separate pane-scoped Kitty remote-control helper, and omitting
that helper makes the session view-only. Presenter discovery is likewise local
and session-scoped: applications pay only a one-second socket existence check
until a server is actually waiting.

## Remote Chrome pixel pane

`tools/kmx_remote_chrome.py` is an optional Linux SSH-command helper for one
interactive Chrome pixel pane. It starts the server on loopback for carriage
through an SSH tunnel, gives Chrome a private X display, fits its window to the
framebuffer, and routes authenticated controller input through XTest. The
helper and `kmx-serve` must come from the same built checkout.

Its lifecycle rule is deliberately strict: after validating the requested
profile, Kilix source root, required programs, dimensions, port, and token, it
stops every live process named `chrome` that it can see and refuses to start
unless the live count reaches zero. Do not use it when another Chrome session
must remain running.

Pass `--kilix-home KILIX_SOURCE_ROOT`, or set `KILIX_HOME`; there is no
working-directory-dependent fallback. With no `--user-data-dir`, the helper
uses and removes an ephemeral profile. To use saved logins, pass an existing
Chrome user-data directory and optionally `--profile-number N`: zero selects
`Default`, and a positive number selects `Profile N`. The helper snapshots and
restores Chrome's general `last_used` and `last_active_profiles` selection so
that the per-session choice does not become the usual profile.

Keep the SSH destination, checkout locations, Chrome data location, chosen
profile, launch wrappers, and logs in operator-owned configuration outside
this checkout, as described above.

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

## What works today

- **`kmx_term`** — a server-side terminal model fed with a pane's raw output.
  It answers *what is on screen now*, which is what lets a peer that missed
  everything be brought current in one message.
- **`kmx_grid`** — the cell snapshot: characters, attributes, colours, widths,
  cursor.
- **`kmx_cells_encode` / `kmx_cells_apply`** — the cell plane. The wire form is
  a **diff against a state the peer is known to hold**, never a log of what
  happened. A peer that missed a megabyte of scrollback is caught up by one
  message describing the screen it should now have, and a lost message is
  superseded rather than retransmitted. The test suite asserts this directly:
  600 KB of skipped output costs under 8 KB to catch up on.
- **Still graphics** — Kitty graphics escapes are collected whole and *in
  wire order relative to the cells around them*, so an image placement and the
  text beside it cannot be reordered. Sequences split across reads are
  reassembled, content-addressed, sent once, and replayed from each client's
  bounded cache on repeat.
- **Motion and audio** — RGB frames are delta-compressed per client and audio
  is framed as timestamped 20 ms PCM blocks. Both are droppable planes: a slow
  client loses stale media rather than stalling lossless text. A private X
  display remains available as the standalone pixel source; live Kilix uses
  the non-blocking presenter tap.
- **Live pane source and pixel input** — `--broker-session` starts the public
  broker-v2 observer CLI outside the scheduler and holds no PTY control slot.
  A separate `--input-command` is the only possible live input descriptor.
  Pixel panes can use the same option; their helper inherits only the private
  X display created for that pane. Host-local `t=s`, `t=f` and `t=t` graphics
  references are suppressed when the tap supplies portable pixels, because
  replaying their names on another host cannot work.
- **Layout plane** — where the panes are, in a few dozen bytes. A four-pane
  arrangement encodes in under 120 bytes, and because the client knows the
  geometry it draws the dividers and title bars itself rather than receiving
  pictures of them. Sent only when the arrangement changes.
- **Still-graphics plane** — Kitty graphics carried under a content-addressed
  cache, so an image that a client already holds costs a reference rather than
  a retransmission.
- **Motion plane** — a pane whose content is pixels, sent as lossless
  rectangles under a rate allowance. The one plane allowed to drop: a frame
  that does not fit is discarded rather than queued, because the next frame
  supersedes it.
- **Audio plane** — PCM blocks with presentation timestamps against one session
  clock, taking their allowance first and fixed, so sound never waits on video.
- **`kmx-serve` / `kmx-attach`** — a multi-pane session served over an
  owner-only Unix socket or a TCP port, and a client that composites and
  renders it, with an incremental framer, a diffing renderer, predictive local
  echo, reconnection, local motion presentation and audio playback. TCP binds
  loopback unless `--lan` is given, always requires a token, and encrypts
  non-loopback traffic by default with a pinned certificate.

## Limitations worth knowing before you start

- **A pixel pane is the whole session.** `--pixel-pane` and `--pane` are
  mutually exclusive: you get a session of text panes, or one pixel pane, not a
  layout mixing them. Live `--broker-session` is also one selected pane.
  Mixing native text and pixel panes in one served layout is a refinement the
  plane model allows and the implementation does not do yet.
- **A pixel pane is view-only unless it has an input helper.**
  `--input-command` receives authenticated controller input on stdin and runs
  with `DISPLAY` set to the pixel pane's private X server. `kmx-attach
  --pixel-input` requests CSI-u keyboard and SGR-pixel mouse reports and scales
  mouse coordinates when the local placement and remote framebuffer differ.
- **A standalone child lives and dies with the server.** If `kmx-serve` owns
  the command itself, exiting the server ends it. A live `--broker-session`
  pane is different: `kitty-pty-broker` keeps owning the PTY and the local
  frontend stays attached when the observer server exits. Client
  `--reconnect` reconnects to a running server; it does not restart one.
  Checked rather than assumed: killing the observer with SIGKILL left the
  broker session alive, and a fresh observer replayed output produced before
  the kill.
- **The planes are not synchronised.** Motion and audio are carried
  independently and neither waits on the other, which is what keeps a stalling
  video from stuttering the sound — but it also means they are not aligned. The
  motion plane carries no presentation timestamp at all, so a client renders
  frames on arrival. For a pane that makes notification sounds this is
  invisible; for watching video with a soundtrack it will drift. Scoped, with
  the measurements, in the project's research notes under `AV_SYNC_SCOPE.md`.
- **Only tapped local graphics are portable.** Inline Kitty graphics remain
  semantic. Host-local `t=s`, `t=f` and `t=t` references are suppressed when
  the presenter tap supplies the corresponding RGB, but an unrelated producer
  outside that tap cannot have an already-consumed local payload reconstructed.
- **The TLS identity is now persisted** in the user's XDG state directory
  (0600 in a 0700 directory), so a pinned fingerprint stays valid across
  restarts. It used to be minted per start, which meant re-copying the
  fingerprint constantly and, in practice, learning to skip the check.
  `--tls-ephemeral` restores the old behaviour. The trade is explicit: the
  private key is on disk now, so it is exposed to backups in a way it was not
  before.
- **Eight client slots.** A connection that has not authenticated within
  `KMX_SETTLE_MS` is dropped, and when every slot is taken a new connection
  displaces the *oldest un-authenticated* one — so peers that connect and stay
  silent can no longer lock a session out, which they previously could, for
  free, from anywhere that could reach the port. A peer that has authenticated
  is never displaced. Eight simultaneous *legitimate* clients is still the
  ceiling.

## Properties the tests assert

Losslessness is the gate this design stands or falls on, so it is checked
rather than argued: every step of every roundtrip test encodes a diff, applies
it to a separate receiver grid, and asserts the two agree exactly — across
text, attributes, colours, scrolling, resize, wide characters, combining
marks, emoji, and 400 steps of randomised cursor motion and erases.

The decoder is the parser a remote peer feeds directly, so it gets more than
tests: every proper prefix of a valid message must be rejected, single-byte
corruption must be caught or safely absorbed, trailing bytes are an error, and
`make fuzz` runs libFuzzer against it under ASan and UBSan.

A peer's claimed grid size is an instruction to allocate, so it is bounded
(`KMX_MAX_DIMENSION`, `KMX_MAX_CELLS`) rather than trusted.

## Dependencies

`libvterm` (MIT) is vendored **unmodified** under `third_party/libvterm` with
its upstream commit and checksums recorded. `make check-vendor` proves the tree
still matches. Upstream already surfaces Kitty graphics APC sequences in wire
order through `vterm_screen_set_unrecognised_fallbacks`, so this project
carries no patches against it.

The native build uses zstd, zlib and OpenSSL. `Xvfb` and `ffmpeg` are
optional standalone pixel-pane dependencies. Live attachment expects the
protocol-v2 `kitty-pty-broker` executable and the local presenter tap that
Kilix pins; neither is copied into this repository. There is no transport
library, video encoder or audio codec dependency — the motion and audio planes
carry lossless rectangles and PCM under zstd. Adding a lossy codec later is a
codec choice, not a change of shape.

## License

MIT.

## Status

Published 2026-07-29. It works, it is tested, and it measures well — see
`SECURITY.md` for what is enforced and, more usefully, for the list of things
that have gone wrong and how they were found.

One thing to be plain about before you point it at anything: **the network path
has not received an independent review.** Treat a reachable bind accordingly —
prefer an SSH tunnel to a loopback port over `--lan`, which is what the
documentation recommends anyway.
