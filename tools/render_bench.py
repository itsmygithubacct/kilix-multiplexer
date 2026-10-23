#!/usr/bin/env python3
"""Measure one real attach client; verify its complete terminal grid.

No observer connection, GPU timing, prediction, or model inference. PTY read
completion is timestamped BEFORE native parsing/verification. The parser still
adds service time between reads; report it separately, never subtract it from
latency. Reference screens are prepared before sending measured input. Uses the
same native terminal library as the host, not independent emulator conformance.
"""
import argparse
import copy
import ctypes as C
import fcntl
import json
import hashlib
import math
import os
from pathlib import Path
import platform
import pty
import random
import select
import signal
import statistics
import struct
import subprocess
import tempfile
import termios
import time

import coding_bench as cb
from coding_workload import paint_bytes


def stats(values):
    ordered = sorted(values)
    return dict(n=len(values), p50_ms=statistics.median(values),
                p95_ms=ordered[math.ceil(.95 * len(values)) - 1],
                p99_ms=ordered[math.ceil(.99 * len(values)) - 1],
                max_ms=max(values), samples_ms=values)


class Terminal:
    def __init__(self, lib, rows, cols):
        self.lib, self.handle, self.grid = lib, C.c_void_p(), cb.Grid()
        cb.Receiver.check(lib.kmx_term_create(C.byref(self.handle), rows, cols))

    def feed(self, data):
        cb.Receiver.check(self.lib.kmx_term_feed(self.handle, data, len(data)))
        cb.Receiver.check(self.lib.kmx_term_snapshot(self.handle, C.byref(self.grid)))

    def close(self):
        self.lib.kmx_grid_free(C.byref(self.grid))
        self.lib.kmx_term_free(self.handle)


def library(path):
    lib = cb.load_library(path)
    signatures = {
        'kmx_term_create': ([C.POINTER(C.c_void_p), C.c_int, C.c_int], C.c_int),
        'kmx_term_feed': ([C.c_void_p, C.c_void_p, C.c_size_t], C.c_int),
        'kmx_term_snapshot': ([C.c_void_p, C.POINTER(cb.Grid)], C.c_int),
        'kmx_term_free': ([C.c_void_p], None),
        'kmx_grid_free': ([C.POINTER(cb.Grid)], None),
        'kmx_grid_equal': ([C.POINTER(cb.Grid), C.POINTER(cb.Grid)], C.c_bool),
    }
    for name, (args, result) in signatures.items():
        getattr(lib, name).argtypes = args
        getattr(lib, name).restype = result
    return lib


def run(args, rows, cols, repetition):
    config = argparse.Namespace(server=args.server, remote=args.remote,
        remote_root=args.remote_root, rows=rows, cols=cols, reliable_input=True)
    lib = library(args.library)
    actual, expected = Terminal(lib, rows, cols), Terminal(lib, rows, cols)
    session = process = tunnel = None
    master = slave = None
    parsing, latency, output_bytes = [], [], 0
    started = time.monotonic()
    try:
        session = cb.Session(config)
        with tempfile.TemporaryDirectory(prefix='kmx-render-bench-') as folder:
            endpoint = session.endpoint
            if args.remote:
                endpoint = str(Path(folder) / 'forward.sock')
                tunnel = subprocess.Popen(['ssh', '-N', '-T', '-o', 'BatchMode=yes',
                    '-o', 'ExitOnForwardFailure=yes', '-L',
                    f'{endpoint}:{session.endpoint}', args.remote], stderr=subprocess.PIPE)
                deadline = time.monotonic() + 10
                while not Path(endpoint).exists():
                    if tunnel.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('SSH forwarding failed')
                    time.sleep(.01)
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
            original = termios.tcgetattr(slave)
            with tempfile.TemporaryFile() as log:
                expected.feed(paint_bytes(0, 0, rows, cols))
                initial_start = time.perf_counter_ns()
                process = subprocess.Popen([str(args.attach), '--socket', endpoint,
                    '--no-predict', '--no-audio', '--reliable-input'],
                    stdin=slave, stdout=slave, stderr=log)
                os.set_blocking(master, False)

                def wait_screen(timeout=15):
                    nonlocal output_bytes
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            log.seek(0)
                            raise RuntimeError(f'attach exited: {log.read().decode()}')
                        if not select.select([master], [], [], .1)[0]:
                            continue
                        data = os.read(master, 65536)
                        received = time.perf_counter_ns()
                        if not data:
                            raise RuntimeError('attach PTY closed')
                        output_bytes += len(data)
                        begin = time.perf_counter_ns()
                        actual.feed(data)
                        equal = lib.kmx_grid_equal(C.byref(actual.grid), C.byref(expected.grid))
                        parsing.append((time.perf_counter_ns() - begin) / 1e6)
                        if equal:
                            return received
                    raise TimeoutError('complete client screen did not match expected grid')

                initial_ms = (wait_screen() - initial_start) / 1e6
                cpu_start = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
                measured_start = time.perf_counter_ns()
                for sequence in range(1, args.warmup + args.samples + 1):
                    expected.feed(paint_bytes(sequence, 0, rows, cols, full=False))
                    sent = time.perf_counter_ns()
                    command = f'EDIT {sequence}\n'.encode()
                    if os.write(master, command) != len(command):
                        raise RuntimeError('partial input write')
                    done = wait_screen()
                    if sequence > args.warmup:
                        latency.append((done - sent) / 1e6)

                # First observe the new sequence, then stall our transport.
                # A separate control channel waits for the host workload to
                # finish, so remaining generation is not called recovery.
                sequence += 1
                expected.feed(paint_bytes(sequence, 1, rows, cols))
                os.write(master, f'BURST {sequence} 300 0.002\n'.encode())
                # Wait for any bytes from the new burst; do not require tick 1
                # (the server may coalesce it). This confirms input delivery.
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if select.select([master], [], [], .1)[0]:
                        data = os.read(master, 65536)
                        output_bytes += len(data)
                        actual.feed(data)
                        header = ''.join(chr(actual.grid.cells[i].chars[0] or 32)
                                         for i in range(cols))
                        if header.startswith(f'CONFIRMED {sequence:08d} '):
                            break
                else:
                    raise TimeoutError('burst input was not delivered')
                stalled = tunnel if tunnel else process
                os.kill(stalled.pid, signal.SIGSTOP)
                paused = time.perf_counter_ns()
                try:
                    done_path = str(Path(session.endpoint).with_name('child.done'))
                    control = """
import json,sys,time
from pathlib import Path
p=Path(sys.argv[1]); seq=int(sys.argv[2]); deadline=time.monotonic()+20
while time.monotonic()<deadline:
    if p.exists():
        d=json.loads(p.read_text())
        if d['sequence']==seq and d['tick']==300: break
    time.sleep(.01)
else: raise TimeoutError('host workload did not complete')
"""
                    if args.remote:
                        waiter = cb.ssh_python(args.remote, control, done_path, sequence)
                        try:
                            _, error = waiter.communicate(timeout=25)
                            if waiter.returncode:
                                raise RuntimeError(error.decode())
                        finally:
                            cb.stop_process(waiter)
                    else:
                        subprocess.run(['python3', '-c', control, done_path, str(sequence)],
                                       check=True, timeout=25)
                    remaining = .5 - (time.perf_counter_ns() - paused) / 1e9
                    if remaining > 0:
                        time.sleep(remaining)
                    expected.feed(paint_bytes(sequence, 300, rows, cols))
                finally:
                    os.kill(stalled.pid, signal.SIGCONT)
                resumed = time.perf_counter_ns()
                recovered = wait_screen()
                recovery = dict(host_complete_before_resume=True,
                    stalled_component='SSH tunnel' if tunnel else 'attach process',
                    stall_ms=(resumed-paused)/1e6,
                    recovery_ms=(recovered-resumed)/1e6)
                cpu_end = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()
                cpu_seconds = sum(int(cpu_end[i])-int(cpu_start[i]) for i in (11,12)) / os.sysconf('SC_CLK_TCK')
                measured_seconds = (time.perf_counter_ns()-measured_start)/1e9
                peak_rss = next(int(line.split()[1]) for line in
                    Path(f'/proc/{process.pid}/status').read_text().splitlines()
                    if line.startswith('VmHWM:'))
                os.write(master, b'\x1d')
                process.wait(timeout=5)
                if process.returncode != 0 or termios.tcgetattr(slave) != original:
                    raise RuntimeError('detach failed to restore terminal')
        return dict(verdict='pass', screen=[rows, cols], repetition=repetition,
                    latency=stats(latency), validation=stats(parsing), recovery=recovery,
                    attach_cpu_seconds=cpu_seconds, attach_measured_seconds=measured_seconds,
                    attach_peak_rss_kib=peak_rss,
                    initial_screen_ms=initial_ms, terminal_bytes=output_bytes, wall_seconds=time.monotonic()-started)
    finally:
        if process and process.poll() is None:
            cb.stop_process(process)
        for fd in (master, slave):
            if fd is not None:
                os.close(fd)
        if tunnel:
            cb.stop_process(tunnel)
        if session:
            session.close()
        actual.close()
        expected.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=cb.ROOT/'build/kmx-serve')
    parser.add_argument('--attach', type=Path, default=cb.ROOT/'build/kmx-attach')
    parser.add_argument('--library', type=Path, default=cb.ROOT/'build/libkilix-mux.so')
    parser.add_argument('--remote')
    parser.add_argument('--remote-root')
    parser.add_argument('--baseline-server', type=Path)
    parser.add_argument('--baseline-attach', type=Path)
    parser.add_argument('--baseline-remote-root')
    parser.add_argument('--screens', default='24x80,57x212,100x300,160x600')
    parser.add_argument('--samples', type=int, default=100)
    parser.add_argument('--warmup', type=int, default=10)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if min(args.samples, args.repetitions) <= 0 or args.warmup < 0:
        parser.error('samples/repetitions must be positive; warmup must be nonnegative')
    if args.remote and not args.remote_root:
        parser.error('--remote requires --remote-root')
    if args.baseline_attach and args.remote and not args.baseline_remote_root:
        parser.error('remote comparison requires --baseline-remote-root')
    screens = [tuple(map(int, x.split('x'))) for x in args.screens.split(',')]
    if any(len(s) != 2 or s[0] < 3 or s[1] < 40 for s in screens):
        parser.error('screens must be ROWSxCOLS, at least 3x40')
    jobs = [(r, c, n) for n in range(args.repetitions) for r, c in screens]
    rng = random.Random(args.seed)
    rng.shuffle(jobs)
    report = dict(schema=2, scope='input write to PTY read completing exact viewport; excludes GPU',
                  observer_connections=0, prediction=False, warmup=args.warmup,
                  seed=args.seed, platform=platform.platform(),
                  binaries={k:str(getattr(args,k).resolve()) for k in ('server','attach','library')},
                  remote=args.remote, remote_root=args.remote_root, results=[])
    try:
        report['sha256'] = {k: hashlib.sha256(getattr(args, k).read_bytes()).hexdigest()
                            for k in ('server', 'attach', 'library')}
        for rows, cols, repetition in jobs:
            variants = ['candidate', 'baseline'] if args.baseline_attach else ['candidate']
            rng.shuffle(variants)
            for variant in variants:
                config = copy.copy(args)
                if variant == 'baseline':
                    config.attach = args.baseline_attach
                    config.server = args.baseline_server or args.server
                    config.remote_root = args.baseline_remote_root or args.remote_root
                result = run(config, rows, cols, repetition)
                result['variant'] = variant
                result['attach_sha256'] = hashlib.sha256(config.attach.read_bytes()).hexdigest()
                result['server_sha256'] = (None if config.remote else
                    hashlib.sha256(config.server.read_bytes()).hexdigest())
                result['remote_root'] = config.remote_root
                report['results'].append(result)
                print(f'{variant} {rows}x{cols} run {repetition}: '
                      f'p50={result["latency"]["p50_ms"]:.2f} '
                      f'p95={result["latency"]["p95_ms"]:.2f} ms', flush=True)
                args.output.write_text(json.dumps(report, indent=2)+'\n')
    except Exception as exc:
        report['error'] = str(exc)
        args.output.write_text(json.dumps(report, indent=2)+'\n')
        raise


if __name__ == '__main__':
    main()
