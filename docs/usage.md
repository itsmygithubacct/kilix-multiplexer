# Using kmx-serve and kmx-attach

Where local configuration belongs, what the client does, how media are
played, serving a live Kilix pane, and the remote Chrome pixel-pane helper.
The optional compressed-audio build (`ENCODEC=1`) is described in
[encodec-audio.md](encodec-audio.md).

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
