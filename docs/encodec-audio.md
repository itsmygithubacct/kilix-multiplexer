# Compressed audio build (`ENCODEC=1`)

The optional `ENCODEC=1` build of `kmx-serve` and `kmx-attach`: codec selection,
epoch-profile negotiation, admission of installed models, threading and
queues, shared encoders, and the transport checks. Counter output is in
[encodec-counters.md](encodec-counters.md).

An optional EnCodec build adds `--audio-codec auto|encodec|pcm` and
`--audio-bitrate 3|6|12` to both programs. The default `auto` keeps PCM for
legacy peers, missing installed mono models and incompatible rate offers.
`encodec` requires the exact mono profile locally and at the peer; an old peer
that cannot select it is refused. `pcm` always selects the existing PCM plane.
After a capability refusal, that connection receives no audio; its other
planes may continue. PCM fallback applies only to a legacy peer or an explicit
PCM selection.
The two endpoints must select the same bitrate. The stereo file profile is
not a live KMX capability.

New EnCodec peers negotiate the epoch-start profile separately from the existing
audio capability bytes. Both advertise C0 and, when its context is ready,
C5-R4; C5-R4 is selected only when both advertise it. A KAC1-only EnCodec peer
gets C0 on both sides. Legacy PCM peers keep PCM. Each endpoint prewarms at
most two fixed native contexts before accepting audio, so profile selection
does not run model loading or inference inside the connection event loop.

The server reports a precise per-profile first-publication counter boundary and
quiescent final totals; see [EnCodec publication counters](encodec-counters.md).

The optional post-HELLO `AUDIO_PROFILE` message (type13) is exactly12 bytes:
`KEP1`, kind (0 offer,1 selection), three reserved zero bytes, then a big-endian
uint32 value. Offers use capability bits0=C0 and1=C5-R4; C0 is required and
unknown offer bits are ignored. Selection values are markers0=C0 or1=C5-R4;
unknown or unadvertised markers are refused. The offer precedes KAC1. Only a
peer that offered the extension receives a selected marker, before its KAC1
selection. Missing marker means C0. Changed duplicates and profile offers after
KAC1 are refused; identical KAC1 duplicates retain the committed selection.
KAC1's size, reserved zeros and strict codec/rate validation are unchanged.

Normal admission uses `KILIX_CONTENT_ROOT`, or the account's NSS home with
`.local/gpu_terminal/kilix/data/desktop-apps`. Each process checks actual
packaged catalog membership, receipts and the complete installed population
through the native provider's embedded F100 adapter, then passes its sealed
graph descriptors to the strict native loader. Model loading and warm-up
finish before capability negotiation. KMX performs no download and does not
substitute a graph directory or an environment-based readiness claim.

Build the optional tools with `ENCODEC=1`, `ENCODEC_CFLAGS` and
`ENCODEC_LIBS` selecting the reviewed native provider built with `ONNX=1
CONTENT=1` and an exact `CONTENT_SOURCE`/`CONTENT_COMMIT`. The build also needs
native epoch-profile setters, libsamplerate and pthreads. Use a separate `BUILD_DIR=build-encodec` when
switching build modes. The default build retains no native codec dependency.

For local development and oracle tests only,
`--development-encodec-assets /absolute/graphs` explicitly uses native byte
validation without installed admission. It prints a development notice and
is never selected automatically. This path grants no receipt, model supply
decision or release qualification. Normal desktop launchers do not use it.

EnCodec inference uses two ORT threads by default. Both programs accept
`--audio-threads 4` to explicitly allocate four inference threads, including
the inference caller, for the mono codec. This requires a native EnCodec
runtime supporting that allocation. Each endpoint selects its own local
budget; it changes no audio negotiation or wire format. The encoder worker has
a fixed queue for six 40 ms frames to retain timely capture during C5-R4 reset work,
and two fixed output slots. The decoder retains two input slots and two output
slots. Capture conversion also
runs there: 24/44.1/48 kHz interleaved PCM16 with one to eight channels is
averaged with headroom and resampled to 24 kHz mono, accumulating exactly
960 samples. Ordinary codec epochs preserve resampler state. Capture gaps
reset it. The first output sample retains its session PTS; resampler and
capture accumulation add delivery delay. Decoded mono is passed to the
existing sink with `KMX_AUDIO_RATE=24000` and `KMX_AUDIO_CHANNELS=1`.

One encoder is shared across clients at each selected epoch profile and rate.
A late C0 client never changes or resets the shared C5-R4 context; both
contexts receive the same source PTS and route packets only to matching clients.
Late clients wait for that profile's next RESET. Idle contexts receive no
capture work. These fixed contexts add memory and mixed-profile CPU cost;
functional checks alone do not qualify that additional resource budget.
Output packets are
opaque native KMA2 and are never passed through zstd. Queue overflow or work
older than 250 ms abandons only the affected profile's shared epoch and resumes at the next one-second
source boundary with native RESET/DISCONTINUITY flags. That age starts at the
first contributing input offer and includes accumulation, inference and output
queue residence. Advancing a broken boundary invalidates already queued and
in-flight encoder work. A client whose own
application backlog is full or stale abandons only its own epoch. Codec calls,
RTF, queue drops and discontinuities are reported at shutdown; `--dump` also
reports selected codec and verified packet PTS/epoch/flags. KMX's existing
socket flushing coalesces audio with other queued traffic. Token bitrate is
not a measured TLS wire-rate claim.

Each profile has a separate shutdown counter row; one aggregate row includes
every context, including earlier profiles used before a reconnect. Client
decoder input backpressure retains a complete packet in the existing bounded
framer until an input slot is free and temporarily pauses socket reads. It
resumes from the codec event without requiring another network read. Retained
packets keep the oldest contributing read's timestamp, so waiting does not
refresh the 250ms age budget. Decoder input/output slots remain two each.

With the native library, patched ORT, Content assets and licence receipts
already installed, a short two-client transport regression uses synthetic
24kHz mono capture and file sinks, without a playback device:

```sh
python3 -B tests/encodec_transport.py --new-bin /absolute/native-build \
    --only installed-pair6 --seconds 30 --evidence /absolute/new-test-directory
```

It requires C5-R4 at 6kbps with the default two inference threads, all 750
frames at both clients, matching PCM lengths and no codec queue drops. It
does not replace calibration, long-run, fault or hardware acceptance.

`make test` includes bounded capability/refusal controls. An optional real
native check is `build-encodec/test-encodec --development-assets /absolute/graphs`.
With the same native build options, build `build-encodec/test-encodec-age` and
run it with `/absolute/graphs` to check total acquisition age at all three
capture rates and recovery after queued or in-flight epoch invalidation.
This test injects scheduling delays around actual native calls.
`tests/encodec_transport.py` checks actual capture, decoder/sink bytes, PTS,
TLS and both legacy directions using synthetic PCM; its arguments select
exact binaries, local graphs and a new evidence directory. These functional
controls do not establish listening, consumer timing, multi-client wire
capacity, memory fit or soak qualification. Motion still has no presentation
timestamp, so this audio feature makes no A/V synchronization claim.
`tests/encodec_profiles.py` separately checks actual extension ordering,
duplicates, C0 fallback and bounded malformed-message refusals. Omitting its
`--assets` argument uses already-admitted installed assets.
