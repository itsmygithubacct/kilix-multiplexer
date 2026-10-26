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

## Epoch-start ages

After each profile's `event=end` record and before its `encodec_profile`
summary, the server writes one `KMX_ENCODEC_EPOCH_AGE v=1` line with
`profile`, `instance=1`, `delivered`, `undelivered`, `max_ms`, `bucket_ms=10`
and `buckets`, 26 comma-separated counts. Every RESET packet that finishes
encoding is one sample, for C0 and C5-R4 alike. When the scheduler receives it,
its age is the time since the oldest capture block it contains arrived, and it
is counted in bucket `age / 10`; the last bucket holds exactly 250 ms, the
largest age the stale budget admits. A RESET packet that is never handed to
the scheduler, because it finished after the budget, went stale in the output
ring or was invalidated by a later epoch break, is `undelivered`. Packets
still queued when the worker stops are not samples.

A p99 over these samples must rank undelivered packets above every bucket:
they are epoch starts that no peer received within the budget. A RESET that
never finished encoding is already an input or output drop and is not a
sample. The age ends at the scheduler's hand-off, before socket queueing and
network transfer, so it does not include client transport or decoding.

`make test-boundary` exercises the actual worker, output ring, accounting lock
and server final-log function with a deterministic native ABI double. It also
drives delivered, stale and invalidated RESET packets through the age record
and checks the server's age line. It uses
native headers and libsamplerate, opens no model or installed admission path,
and is neither codec accuracy nor timing qualification. Ordinary native builds
continue to link the installed EnCodec library.
