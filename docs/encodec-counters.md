# EnCodec publication counters

Each server process owns one encoder per epoch profile (C0 and C5-R4).
`KMX_ENCODEC_PHASE v=1` on stderr records each encoder's first output-ring
publication and its final, quiescent counters. Lifetime counters are retained
in the existing profile and aggregate shutdown lines.

The `event=start` snapshot is captured immediately before the worker releases
its first packet into the scheduler's output ring. Every counter update and
the snapshot share one short mutex. Inference, resampling, polling and I/O
never hold that mutex. Consequently a counter update is wholly before or after
the boundary; reading independent atomics at the client or after warm-up
cannot provide this ordering.

The start fields are `profile`, `instance=1`, monotonic `at_ns`, the first
packet's `pts_ms`, `epoch`, `flags`, and `calls`, `inference_ns`, `input_drops`,
`output_drops`, `discontinuities`. The server emits that immutable record
before queueing the first received packet to peers, or at shutdown if no packet
survives receive filtering. The boundary precedes transport delivery: stale or
invalidated packets, queue drops and later resets remain observable after it.

At shutdown the scheduler stops and joins the worker before reading its final
counters. `event=end` includes monotonic `at_ns`, `started=0|1`, the same counter
fields and lifetime `client_drops`. An unused profile has `started=0` and no
start record. Per-profile post-publication counters are final minus start.
Client drops have no startup subtraction. A reconnect, idle interval or encoder
epoch reset never replaces the snapshot. A new process requires new evidence.

Consumers must reject missing, duplicate, malformed, overflowing, nonmonotonic
or incomplete records, inconsistent profile/aggregate totals and unattributed
profiles. The snapshot excludes only accounting commits before the producer's
first publication; it does not excuse losses later hidden by a client's readiness
window. A trace without a usable start has no post-publication counter claim.

`make test-boundary` exercises the actual worker, output ring, accounting lock
and server final-log function with a deterministic native ABI double. It uses
native headers and libsamplerate, opens no model or installed admission path,
and is neither codec accuracy nor timing qualification. Ordinary native builds
continue to link the installed EnCodec library.
