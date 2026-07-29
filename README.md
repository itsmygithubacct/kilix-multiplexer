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

Design notes are maintained separately from this release tree.

**Status: the text and layout planes work end to end.** A multi-pane session
can be served and attached over a socket, with predictive local echo and
client-drawn chrome. Still missing: the graphics, motion and audio planes,
multi-client attach, and any transport beyond a Unix socket.

```sh
make               # library and tools
make test          # unit suite
make sanitize      # under ASan + UBSan
make fuzz          # libFuzzer against the decoder (needs clang)
make check-vendor  # the vendored tree is unmodified upstream
tests/integration.sh   # real processes over a real socket
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

The client renders the screen, forwards typing, follows resizes, and echoes
keystrokes optimistically — underlined until the server confirms them.
`--no-predict` turns that off; `--dump` renders without taking over the
terminal, which is how the integration tests drive it.

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
- **Graphics capture** — Kitty graphics escapes are collected whole and *in
  wire order relative to the cells around them*, so an image placement and the
  text beside it cannot be reordered. Sequences split across reads are
  reassembled. They are captured but not yet carried: that is the still and
  motion planes, which come later.
- **Layout plane** — where the panes are, in a few dozen bytes. A four-pane
  arrangement encodes in under 120 bytes, and because the client knows the
  geometry it draws the dividers and title bars itself rather than receiving
  pictures of them. Sent only when the arrangement changes.
- **`kmx-serve` / `kmx-attach`** — a multi-pane session served over an
  owner-only socket and a client that composites and renders it, with an
  incremental framer, a diffing renderer, and predictive local echo.

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

`libvterm` (MIT), vendored **unmodified** under `third_party/libvterm` with its
upstream commit and checksums recorded. `make check-vendor` proves the tree
still matches. Upstream already surfaces Kitty graphics APC sequences in wire
order through `vterm_screen_set_unrecognised_fallbacks`, so this project
carries no patches against it.

Nothing else. No transport library, no compressor, and no terminal ownership
yet — those arrive with the phases that need them.

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
