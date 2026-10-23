#!/usr/bin/env python3
"""Native stage timings and payload sizes, with full-grid round-trip checks."""
import ctypes as C, json, time, random, sys
from pathlib import Path
from bench_native_support import bindings, RenderedReceiver, Buffer, init_stats, metrics, cb, distribution, load_comparator
import argparse
parser = argparse.ArgumentParser(description='Native CPU and payload benchmark; synthetic clock, not network FPS')
parser.add_argument('--library', type=Path, default=Path('build/libkilix-mux.so'))
parser.add_argument('--receiver-library', type=Path, help='Optional older library for wire compatibility checks')
parser.add_argument('--comparator', type=Path, default=Path('build/bench-grid.so'))
parser.add_argument('--samples', type=int, default=100)
parser.add_argument('--warmup', type=int, default=10)
parser.add_argument('--repetitions', type=int, default=3)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if min(args.samples, args.repetitions) <= 0 or args.warmup < 0:
    parser.error('invalid sample counts')
load_comparator(args.comparator)
lib = bindings(cb.load_library(args.library.resolve()))
decoder = bindings(cb.load_library(args.receiver_library.resolve())) if args.receiver_library else lib
results = []
jobs = [(r, c, w, n) for n in range(args.repetitions) for r, c in ((24, 80), (57, 212), (100, 300), (160, 600)) for w in ('sparse', 'scroll', 'full-redraw', 'unicode-style', 'alternate-erase')]
random.Random(42).shuffle(jobs)
for rows, cols, workload, repetition in jobs:
    init_stats()
    sync = C.c_void_p()
    assert lib.kmx_sync_create(C.byref(sync), rows, cols) == 0
    lib.kmx_sync_set_interval(sync, 1)
    receiver = RenderedReceiver(decoder, rows, cols)
    wire = Buffer()
    produced = C.c_bool()
    feed_times = []
    encode_times = []
    input_bytes = wire_bytes = 0
    frames = args.samples + args.warmup
    try:
        for frame in range(frames):
            if frame == args.warmup:
                init_stats()
                feed_times = []
                encode_times = []
                input_bytes = wire_bytes = 0
            rng = random.Random(frame)
            if workload == 'sparse':
                data = f'\x1b[{frame % rows + 1};{frame % max(1, cols - 1) + 1}H{chr(65 + frame % 26)}'.encode()
            elif workload == 'scroll':
                data = (f'\x1b[{rows};1H' if frame == 0 else '') + f'log {frame:05d} ' + 'payload ' * max(1, (cols - 20) // 8) + '\r\n'
                data = data.encode()
            elif workload == 'full-redraw':
                data = ('\x1b[?25l' + ''.join((f'\x1b[{r + 1};1H\x1b[38;2;{(frame * 13 + r) % 256};100;200m' + ''.join(rng.choices('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', k=cols - 1)) for r in range(rows))) + '\x1b[0m\x1b[H').encode()
            elif workload == 'unicode-style':
                line = 'café 日本 é ' * max(1, (cols - 12) // 12)
                data = ('\x1b[2J' + ''.join((f'\x1b[{r + 1};1H\x1b[1;3;4;38;2;{frame % 256};80;180m{frame:04d} {line}\x1b[0m' for r in range(rows))) + '\x1b[H').encode()
            else:
                data = (('\x1b[?1049h' if frame % 2 == 0 else '\x1b[?1049l') + '\x1b[2J\x1b[H' + f'screen {frame}' + f'\x1b[{rows};1Hbottom {frame}\x1b[H').encode()
            input_bytes += len(data)
            t = time.perf_counter_ns()
            assert lib.kmx_sync_feed(sync, data, len(data)) == 0
            feed_times.append((time.perf_counter_ns() - t) / 1000000.0)
            lib.kmx_buffer_reset(C.byref(wire))
            now = 1000000 + frame * 1000
            t = time.perf_counter_ns()
            assert lib.kmx_sync_poll(sync, now, C.byref(wire), C.byref(produced), None) == 0
            encode_times.append((time.perf_counter_ns() - t) / 1000000.0)
            if produced.value:
                wire_bytes += wire.size
                seq = receiver.apply(C.string_at(wire.data, wire.size))
                assert lib.kmx_grid_equal(lib.kmx_sync_current(sync), receiver.lib.kmx_receiver_grid(receiver.handle)), 'host/decoder mismatch'
                assert lib.kmx_sync_ack_at(sync, seq, now) == 0
        result = {
            'screen': [rows, cols],
            'workload': workload,
            'verdict': 'pass',
            'frames': args.samples,
            'warmup': args.warmup,
            'repetition': repetition,
            'pty_bytes': input_bytes,
            'wire_bytes': wire_bytes,
            'reduction': input_bytes / wire_bytes if wire_bytes else None,
            'host_feed': distribution(feed_times),
            'host_encode': distribution(encode_times),
            'client': metrics(),
        }
    except Exception as e:
        result = {
            'screen': [rows, cols],
            'workload': workload,
            'verdict': 'fail',
            'frame': frame,
            'error': str(e),
            'client': metrics(),
        }
    finally:
        receiver.close()
        lib.kmx_sync_free(sync)
        lib.kmx_buffer_free(C.byref(wire))
    results.append(result)
    print(rows, cols, workload, result['verdict'], result.get('error', ''), flush=True)
    args.output.write_text(json.dumps(results, indent=2) + '\n')
args.output.write_text(json.dumps(results, indent=2) + '\n')
if any((x['verdict'] != 'pass' for x in results)):
    sys.exit(1)
