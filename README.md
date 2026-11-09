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
Ships in Plebian-OS 0.2.3 as a submodule of
[Kilix](https://github.com/itsmygithubacct/kilix#readme)
(`third_party/kilix-multiplexer`). Kilix builds it on first use of
`kilix remote serve` / `kilix remote attach`, which serve and attach a live
Kilix pane.

**Status: all six planes work end to end** — layout, cells, stills, motion,
audio, and input. A multi-pane session can be served over a Unix socket or TCP,
with TLS and token authentication, attached by several clients at once, with
predictive local echo, client-drawn chrome, and reconnection. Verified between
two machines in both directions. A live Kilix pane can also be observed through
`kitty-pty-broker` v2 while RGB frames arrive through the separate
`kitty-frame-presenter` tap; the observer itself remains read-only.

## What it does

- Serves one or more panes (`kmx-serve`) over an owner-only Unix socket or a
  TCP port, and attaches to them (`kmx-attach`) from another terminal.
- Carries six typed planes: layout, cells (lossless diffs against the state
  the peer holds), still graphics (content-addressed), motion (droppable
  pixel rectangles), audio (timestamped PCM, optionally compressed) and input.
- Keeps typing responsive with predictive local echo, and reconnects a
  client to a running server.
- Requires a token on TCP, binds loopback unless `--lan` is given, and uses
  TLS with a pinned, persisted certificate for non-loopback traffic.
- Observes a live Kilix pane through a `kitty-pty-broker` protocol-v2
  observer and receives its pixels through the `kitty-frame-presenter` tap.

## Build and test

```sh
make               # library and tools
make test          # unit suite
make test-coding    # coding-session PTYs, input backpressure, client modes
make sanitize      # under ASan + UBSan
make fuzz          # libFuzzer against the decoder (needs clang)
make check-vendor  # original upstream hashes plus recorded local patches
tests/integration.sh   # real processes over a real socket
tests/network.sh HOST  # both directions through an SSH tunnel
tests/lan.sh HOST      # direct routed TLS, both directions and all media
tools/bench.sh         # what the cell plane actually costs
```

For coding CLIs, see [coding sessions](docs/coding-sessions.md) for the
terminal-mode extension, delayed-ACK repair, input delivery guarantees,
repeatable SSH benchmarks, and remaining history/capability work.

The native build needs zstd, zlib and OpenSSL; `Xvfb` and `ffmpeg` are optional
for standalone pixel panes. See [docs/design.md](docs/design.md#dependencies).

## Try it

```sh
# one pane
kmx-serve --socket ./kmx.sock -- bash
# or several, side by side
kmx-serve --socket ./kmx.sock --split horizontal --pane bash --pane 'top -d1'

kmx-attach --socket ./kmx.sock   # Ctrl-] detaches, Ctrl-O changes pane
```

From Kilix, serve a pane that the PTY broker owns and attach to it:

```sh
kilix remote serve PANE_ID --socket 127.0.0.1:47800
kilix remote attach --socket 127.0.0.1:47800 --token TOKEN_FROM_SERVER
```

## Usage

| Command | What it does |
|---|---|
| `kmx-serve --socket PATH -- COMMAND` | Serve one pane running COMMAND |
| `kmx-serve --socket PATH --split horizontal\|vertical --pane CMD --pane CMD` | Serve several panes side by side |
| `kmx-serve --socket PATH --pixel-pane 'COMMAND'` | Serve one X client from a private display as a pixel pane |
| `kmx-serve --socket PATH --broker-session ID --broker-runtime DIR` | Observe a live protocol-v2 broker session (`KITTY_PTY_BROKER_RUNTIME` can supply the runtime directory) |
| `kmx-serve ... --lan`, `--tls`, `--tls-ephemeral`, `--token` | Reachable TCP bind, TLS and token options |
| `kmx-attach --socket PATH [--token T] [--tls-fingerprint F]` | Attach; `Ctrl-]` detaches, `Ctrl-O` changes pane |
| `kmx-attach ... --no-predict`, `--reconnect SECONDS`, `--dump`, `--no-audio`, `--pixel-input` | Client behaviour |
| `kilix remote serve\|attach\|view` | The Kilix wrapper for a broker-owned pane |

Client options, audio sinks, live Kilix panes and the remote Chrome helper are
in [docs/usage.md](docs/usage.md); the compressed-audio build is in
[docs/encodec-audio.md](docs/encodec-audio.md).

## How it works

```
 server (kmx-serve)                                    client (kmx-attach)
 pane PTY / broker observer ──> kmx_term ──> kmx_grid ─┐
 Kitty graphics escapes ──> content-addressed stills ──┤  ordered KMX stream
 presenter tap / Xvfb ──> motion rectangles (droppable)├── Unix, TCP, TLS ──> render cells,
 capture ──> audio blocks (droppable) ─────────────────┤                      stills, motion,
 layout (pane geometry, sent on change) ───────────────┘ <── input, resize ── audio; predict
```

The server keeps a terminal model of each pane and sends cell diffs against
the screen the client is known to hold, so a lost or skipped stretch of output
is replaced by one message describing the current screen rather than being
retransmitted. Still images keep their order relative to the surrounding text
and are sent once per client cache. Motion and audio are the only planes
allowed to drop, so a slow link loses stale media rather than stalling text.
Motion carries no presentation timestamp, so audio and video are not
synchronised. What each plane does, the current limitations and what the
tests assert are in [docs/design.md](docs/design.md); wire costs and
benchmarks are in [docs/performance.md](docs/performance.md).

## Status

Published 2026-07-29. It works, it is tested, and it measures well — see
`SECURITY.md` for what is enforced and, more usefully, for the list of things
that have gone wrong and how they were found.

One thing to be plain about before you point it at anything: **the network path
has not received an independent review.** Treat a reachable bind accordingly —
prefer an SSH tunnel to a loopback port over `--lan`, which is what the
documentation recommends anyway.

## Documentation

| Document | Contents |
|---|---|
| [docs/usage.md](docs/usage.md) | Local configuration, client behaviour, media playback, live Kilix panes, remote Chrome pixel pane |
| [docs/encodec-audio.md](docs/encodec-audio.md) | The optional compressed-audio build: selection, negotiation, admission, threads, queues, checks |
| [docs/encodec-counters.md](docs/encodec-counters.md) | Per-profile publication counters |
| [docs/coding-sessions.md](docs/coding-sessions.md) | Coding CLIs over KMX: terminal modes, delayed-ACK repair, input delivery, benchmarks |
| [docs/performance.md](docs/performance.md) | Wire cost per workload, rendering and latency measurements |
| [docs/design.md](docs/design.md) | The planes, limitations, tested properties and dependencies |
| [SECURITY.md](SECURITY.md) | What is enforced, and the defects found so far |

## Related repositories

- [kilix](https://github.com/itsmygithubacct/kilix#readme): pins this
  submodule and wraps it as `kilix remote`.
- [kitty-pty-broker](https://github.com/itsmygithubacct/kitty-pty-broker#readme):
  owns the live pane the server observes.
- [kitty-frame-presenter](https://github.com/itsmygithubacct/kitty-frame-presenter#readme):
  its frame tap supplies a live pane's pixels.
- [kilix-encodec](https://github.com/itsmygithubacct/kilix-encodec#readme):
  the native codec library the optional audio build links.

## Part of Plebian-OS

kilix-multiplexer is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It ships inside Kilix as the `third_party/kilix-multiplexer` submodule and is
built when a user first runs `kilix remote`. The map of every repository and
how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT.
