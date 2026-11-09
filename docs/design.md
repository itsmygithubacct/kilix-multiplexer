# Design: planes, limits and tests

What each plane carries, the limitations of the current implementation, what
the tests assert, and the dependencies.

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

`libvterm` (MIT) is vendored under `third_party/libvterm` at the commit recorded
in `UPSTREAM`, with a numbered patch for dimension and allocation guards.
The original upstream checksums remain unchanged. `make check-vendor` reverses
the recorded patch series in a temporary copy and verifies those original
hashes, rejecting unrecorded source changes. This check needs Python 3 and
`patch`. Upstream already surfaces Kitty graphics APC sequences in wire order
through `vterm_screen_set_unrecognised_fallbacks`; no graphics patch is needed.

The native build uses zstd, zlib and OpenSSL. `Xvfb` and `ffmpeg` are
optional standalone pixel-pane dependencies. Live attachment expects the
protocol-v2 `kitty-pty-broker` executable and the local presenter tap that
Kilix pins; neither is copied into this repository. There is no transport
library or video encoder dependency. The default audio build carries PCM
under zstd. The optional EnCodec build additionally uses `libkilix-encodec`,
its ONNX Runtime dependency, libsamplerate and pthreads as described above;
the legacy PCM transport remains available.
