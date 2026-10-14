"""Real optional EnCodec transport controls with synthetic PCM and local graphs.

The ordinary matrix exercises the explicit development path. Installed modes
use caller-supplied installed Content/receipts and two real clients.
Neither mode grants long-run, wire performance or hardware qualification.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def capture(rate, channels, gate, seconds=2):
    until = time.monotonic() + 90
    while not Path(gate).exists():
        if time.monotonic() > until:
            return 1
        time.sleep(.01)
    started_ns = time.monotonic_ns()
    started = started_ns / 1e9
    Path(gate).with_name('capture-origin.json').write_text(
        json.dumps({'monotonic_ns': started_ns, 'rate': rate, 'channels': channels}) + '\n')
    timing = Path(gate).with_name('capture-timing.csv').open('w') if seconds > 60 else None
    writer = csv.writer(timing) if timing else None
    if writer:
        writer.writerow(('packet', 'capture_ns', 'written_ns', 'scheduled_ns'))

    def terminated(_signum, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, terminated)
    try:
        for packet in range(int(seconds * 50)):
            samples = []
            for offset in range(rate // 50):
                index = packet * (rate // 50) + offset
                value = round(6000 * math.sin(index * 2 * math.pi * 440 / rate))
                for channel in range(channels):
                    samples.append(value if channel % 2 == 0 else value // 2)
            captured = time.monotonic_ns()
            sys.stdout.buffer.write(struct.pack('<' + 'h' * len(samples), *samples))
            sys.stdout.buffer.flush()
            if writer:
                writer.writerow((packet, captured, time.monotonic_ns(), started_ns + packet * 20_000_000))
            time.sleep(max(0, started + (packet + 1) * .02 - time.monotonic()))
    finally:
        if timing:
            timing.close()
    return 0


def sink(path):
    """Count/hash the owned PCM pipe, retaining no long-run audio payload."""
    count = 0
    digest = hashlib.sha256()
    while block := sys.stdin.buffer.read(65536):
        count += len(block)
        digest.update(block)
    Path(path).write_text(json.dumps({'clean_eof': True, 'pcm_bytes': count,
                                    'pcm_sha256': digest.hexdigest()}) + '\n')
    return 0


def percentile99(values):
    return sorted(values)[math.ceil(len(values) * .99) - 1]


def measure_calibration(options, process, peers, dumps):
    """Observe only these owned children in the caller's new systemd scope."""
    membership = Path('/proc/self/cgroup').read_text().strip().split('::', 1)[1]
    cgroup = Path('/sys/fs/cgroup') / membership.lstrip('/')
    assert cgroup.name == options.cgroup_unit and cgroup.name.endswith('.scope'), cgroup
    records = [[], []]
    streams = [path.open('rb') for path in dumps]
    pending = [b'', b'']
    timing = [(options.evidence / f'arrivals-{name}.csv').open('w') for name in ('a', 'b')]
    writers = [csv.writer(file) for file in timing]
    for writer in writers:
        writer.writerow(('observed_ns', 'size', 'sequence', 'pts_ms', 'epoch', 'flags'))
    resources = (options.evidence / 'resources.csv').open('w')
    resource_writer = csv.writer(resources)
    resource_writer.writerow(('observed_ns', 'process', 'pid', 'rss_kb', 'peak_rss_kb',
                              'cgroup_current_bytes', 'cgroup_peak_bytes'))
    peaks = {name: 0 for name in ('server', 'healthy-a', 'healthy-b')}
    observations = []
    started = cutoff = None
    sample_at = 0
    status_at = 0
    last_poll = time.monotonic_ns()
    pattern = rb'KMX_AUDIO (\d+) #(\d+) at=(\d+) epoch=(\d+) flags=(\d+)'

    def read_available():
        for index, stream in enumerate(streams):
            data = pending[index] + stream.read(512 * 1024)
            lines = data.split(b'\n')
            pending[index] = lines.pop()
            assert len(pending[index]) <= 4096, 'unterminated audio record'
            observed = time.monotonic_ns()
            for line in lines:
                match = re.search(pattern, line)
                if match:
                    row = (observed, *map(int, match.groups()))
                    records[index].append(row)
                    writers[index].writerow(row)

    def sample(now):
        current = int((cgroup / 'memory.current').read_text())
        peak = int((cgroup / 'memory.peak').read_text())
        for name, child in zip(peaks, [process, *peers]):
            status = Path(f'/proc/{child.pid}/status').read_text()
            rss = int(re.search(r'^VmRSS:\s+(\d+) kB$', status, re.M).group(1))
            high = int(re.search(r'^VmHWM:\s+(\d+) kB$', status, re.M).group(1))
            peaks[name] = max(peaks[name], high)
            resource_writer.writerow((now, name, child.pid, rss, high, current, peak))
        resources.flush()
        for file in timing:
            file.flush()
        assert all(path.stat().st_size <= 48 * 1024 * 1024 for path in dumps), 'dump cap exceeded'

    try:
        readiness_end = time.monotonic_ns() + 90_000_000_000
        while cutoff is None or time.monotonic_ns() < cutoff:
            assert process.poll() is None and all(peer.poll() is None for peer in peers), \
                'owned endpoint ended during calibration'
            read_available()
            now = time.monotonic_ns()
            if started is None and all(records):
                started = max(rows[0][0] for rows in records)
                cutoff = started + options.seconds * 1_000_000_000
            if started is None:
                assert now < readiness_end, 'first C5 frame readiness deadline'
            else:
                observations.append((now - last_poll) / 1e9)
                assert all((now - rows[-1][0]) / 1e9 <= .82 for rows in records), \
                    'actual audio arrival stalled over 0.82 seconds'
                if now >= status_at:
                    print(json.dumps({'calibration_elapsed_seconds': round((now - started) / 1e9, 1),
                                      'observed_frames': [len(rows) for rows in records]}), flush=True)
                    status_at = now + 60_000_000_000
            if now >= sample_at:
                sample(now)
                sample_at = now + 1_000_000_000
            last_poll = now
            time.sleep(.002 if cutoff and cutoff - now < 250_000_000 else .01)
        sample(time.monotonic_ns())
        # Do not credit records first observed at or after the nominal cutoff.
        read_available()
        peak = int((cgroup / 'memory.peak').read_text())
        events = dict(line.split() for line in (cgroup / 'memory.events').read_text().splitlines())
        assert int(events['oom']) == int(events['oom_kill']) == 0, events
        result = {'records': records, 'window_start_ns': started, 'window_end_ns': cutoff,
                'process_peak_rss_kb': peaks, 'cgroup_peak_bytes': peak,
                'cgroup': str(cgroup), 'cgroup_memory_events': events,
                'poll_p99_seconds': percentile99(observations),
                'poll_max_seconds': max(observations)}
        # Retain measured cadence/resources even if later grading rejects it.
        (options.evidence / 'measurement-summary.json').write_text(json.dumps(
            {key: value for key, value in result.items() if key != 'records'}, indent=2) + '\n')
        return result
    finally:
        for file in [*streams, *timing, resources]:
            file.close()


def finish_calibration(options, state, received, dumps, errors):
    """Original loss/tail/arrival limits, plus measured capture-to-RESET age."""
    origin = json.loads((options.evidence / 'capture-origin.json').read_text())['monotonic_ns']
    with (options.evidence / 'capture-timing.csv').open() as handle:
        capture_times = {int(row['packet']): int(row['capture_ns']) for row in csv.DictReader(handle)}
    expected = options.seconds * 25
    clients = []
    largest = 0
    for index, records in enumerate(state.pop('records')):
        # Lifetime integrity and PCM accounting also cover readiness/drain.
        raw = re.findall(r'KMX_AUDIO (\d+) #(\d+) at=(\d+) epoch=(\d+) flags=(\d+)', dumps[index].read_text())
        raw = [tuple(map(int, row)) for row in raw]
        assert raw and raw[0][-1] == 13 and all(row[0] == 1920 and row[-1] in (0, 9, 13) for row in raw)
        assert all(b[1] == a[1] + 1 and b[2] > a[2] and b[3] >= a[3] for a, b in zip(raw, raw[1:])), index
        payload = json.loads(received[index].read_text())
        assert payload['clean_eof'] is True and payload['pcm_bytes'] == len(raw) * 1920, (index, payload)
        # The all-ready snapshot includes every batch already observed. Those
        # records still count for lifetime integrity, not useful-window credit.
        window = [row for row in records if state['window_start_ns'] < row[0] < state['window_end_ns']]
        assert window, index
        gaps = [{'pts': a[3], 'missing_ms': b[3] - a[3] - 40}
                for a, b in zip(window, window[1:]) if b[3] - a[3] != 40]
        maximum = max((sum(t['pts'] - 59999 <= e['pts'] <= t['pts'] for e in gaps)
                       for t in gaps), default=0)
        largest = max(largest, len(gaps))
        deficit = expected - len(window)
        loss = max(0, deficit) / expected
        ages = [(row[0] - capture_times[row[3] // 20]) / 1e9 for row in window if row[5] & 1]
        nominal_ages = [(row[0] - origin - row[3] * 1_000_000) / 1e9 for row in window if row[5] & 1]
        arrival = max((b[0] - a[0]) / 1e9 for a, b in zip(window, window[1:]))
        silence = (state['window_end_ns'] - window[-1][0]) / 1e9
        assert loss <= .01, (index, loss, len(window))
        assert all(0 < gap['missing_ms'] <= 1000 for gap in gaps) and maximum <= 2, (index, gaps)
        assert deficit <= sum(gap['missing_ms'] for gap in gaps) / 40 + 1, (index, deficit, gaps)
        assert arrival <= .82 and silence <= .12 and state['poll_max_seconds'] <= 2, (index, arrival, silence)
        assert ages and all(age >= 0 for age in ages) and percentile99(ages) <= .25, (index, ages[:5])
        clients.append({'client': index, 'window_frames': len(window), 'loss_fraction': loss,
                        'forward_gaps': gaps, 'max_events_in_60s': maximum,
                        'max_arrival_interval_seconds': arrival, 'silence_at_end_seconds': silence,
                        'epoch_start_samples': len(ages), 'epoch_start_age_p99_seconds': percentile99(ages),
                        'epoch_start_age_max_seconds': max(ages),
                        'nominal_epoch_start_age_p99_seconds': percentile99(nominal_ages), **payload})
    stats_pattern = r'kmx-(?:serve|attach): encodec(?:_profile)? calls=\d+ rtf=\S+ input_drops=(\d+) output_drops=(\d+) discontinuities=(\d+)'
    server_text = (options.evidence / 'serve.err').read_text()
    phase = re.search(r'event=start profile=C5-R4 .*?input_drops=(\d+) output_drops=(\d+) discontinuities=(\d+)', server_text)
    server_stats = re.findall(stats_pattern, server_text)
    assert phase and len(server_stats) == 3, server_stats
    post = [int(end) - int(begin) for end, begin in zip(server_stats[-1], phase.groups())]
    assert post[0] == 0 and 0 <= post[1] <= 5 * largest and 0 <= post[2] <= largest, post
    assert all(int(value) == 0 for value in re.findall(r'client_drops=(\d+)', server_text)), server_text
    for path in errors:
        stats = re.findall(stats_pattern, path.read_text())
        assert len(stats) == 3 and all(int(i) == int(o) == 0 for i, o, _d in stats), (path, stats)
    state.update({'passed': True, 'seconds': options.seconds, 'clients': clients,
                  'server_post_publication': dict(zip(('input_drops', 'output_drops', 'discontinuities'), post)),
                  'bounds_1_5x': {'process_rss_kb': {name: math.ceil(peak * 1.5)
                                  for name, peak in state['process_peak_rss_kb'].items()},
                                  'cgroup_bytes': math.ceil(state['cgroup_peak_bytes'] * 1.5)},
                  'scope': 'installed two-client calibration'})
    return state


def stop(process):
    if process is None:
        return
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)
    # Commands in this harness run only inside the owned process group.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_for(check, process, seconds=12):
    deadline = time.monotonic() + seconds
    while not check():
        if process.poll() is not None:
            raise AssertionError('owned process ended before readiness')
        if time.monotonic() > deadline:
            raise AssertionError('readiness deadline exceeded')
        time.sleep(.02)


def run_case(options, name, bitrate, rate, channels, *, tls=False, legacy_server=False, legacy_client=False,
             installed_fallback=False, sink_command=None):
    directory = options.evidence / name
    directory.mkdir()
    process = peer = None
    gate = directory / 'capture-ready'
    received = directory / 'received.pcm'
    # Evidence paths may exceed Unix's sockaddr_un path bound. Keep only the
    # owned live endpoint short; argv and all persistent evidence stay below.
    socket_directory = tempfile.TemporaryDirectory(prefix='kmxe-', dir='/tmp')
    socket_path = Path(socket_directory.name) / 'session.sock'
    serve_bin = options.old_bin if legacy_server else options.new_bin
    attach_bin = options.old_bin if legacy_client else options.new_bin
    command = shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture', str(rate), str(channels), str(gate)])
    server = [str(serve_bin/'kmx-serve'), '--socket', str(socket_path), '--pane', 'sleep 30',
              '--audio-source', 'exec '+command, '--audio-rate', str(rate), '--audio-channels', str(channels)]
    client = [str(attach_bin/'kmx-attach'), '--socket', str(socket_path), '--view', '--dump', '--reconnect', '0',
              '--audio-output', sink_command or 'exec cat > '+shlex.quote(str(received))]
    if not legacy_server:
        server += ['--audio-bitrate', str(bitrate)]
        if not installed_fallback:server += ['--development-encodec-assets', str(options.assets)]
    if not legacy_client:
        client += ['--audio-bitrate', str(bitrate)]
        if not installed_fallback:client += ['--development-encodec-assets', str(options.assets)]
    if tls:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port=probe.getsockname()[1]
        server[server.index(str(socket_path))] = f'127.0.0.1:{port}'
        client[client.index(str(socket_path))] = f'127.0.0.1:{port}'
        server += ['--tls', '--tls-ephemeral']
        # TCP's legacy HELLO requires a token even on this loopback fixture.
        server += ['--token', '22'*16]
        client += ['--token', '22'*16]
    with (directory/'serve.out').open('wb') as sout, (directory/'serve.err').open('wb') as serr, \
         (directory/'attach.out').open('wb') as cout, (directory/'attach.err').open('wb') as cerr:
        try:
            process = subprocess.Popen(server, stdout=sout, stderr=serr, start_new_session=True)
            if tls:
                wait_for(lambda: re.search(r'--tls-fingerprint ([0-9a-f]{64})', (directory/'serve.err').read_text()), process)
                fingerprint = re.search(r'--tls-fingerprint ([0-9a-f]{64})', (directory/'serve.err').read_text()).group(1)
                client += ['--tls-fingerprint', fingerprint]
            else:
                wait_for(socket_path.exists, process)
            peer = subprocess.Popen(client, stdout=cout, stderr=cerr, start_new_session=True)
            if not legacy_client and not legacy_server:
                selected = 'pcm-s16le-zstd-v1' if installed_fallback else 'encodec-24k-mono-v1'
                wait_for(lambda: 'KMX_AUDIO_CODEC '+selected in (directory/'attach.out').read_text(), peer)
            else:
                wait_for(lambda: bool((directory/'attach.out').stat().st_size), peer)
            gate.touch()
            wait_for(lambda: len(re.findall(r'KMX_AUDIO ', (directory/'attach.out').read_text())) >= 40, peer, 8)
            stopped = time.monotonic()
            stop(peer)
            shutdown_seconds = time.monotonic() - stopped
            stop(process)
        finally:
            try:
                stop(peer); stop(process)
            finally:
                socket_directory.cleanup()
    text = (directory/'attach.out').read_text()
    # Dump output also contains terminal rendering escapes; the first audio
    # diagnostic can follow a cursor escape without a preceding newline.
    rows = re.findall(r'KMX_AUDIO (\d+) #\d+ at=(\d+)(?: epoch=(\d+) flags=(\d+))?', text)
    assert len(rows) >= 40, rows
    encodec = not legacy_server and not legacy_client and not installed_fallback
    expected_bytes = 1920 if encodec else rate // 50 * channels * 2
    step = 40 if encodec else 20
    assert all(int(row[0]) == expected_bytes for row in rows)
    points = [int(row[1]) for row in rows]
    assert points[0] == 0 and all(b-a == step for a,b in zip(points, points[1:])), points
    if encodec:
        assert all(row[2] for row in rows)
        assert int(rows[0][3]) & 1
        assert 'input_drops=0 output_drops=0' in (directory/'serve.err').read_text()
        assert 'input_drops=0 output_drops=0' in (directory/'attach.err').read_text()
    payload = received.read_bytes()
    assert len(payload) >= 40 * expected_bytes
    result = {'case':name, 'passed':True, 'bitrate':bitrate, 'capture_rate':rate, 'capture_channels':channels,
              'codec':'encodec' if encodec else 'pcm', 'tls':tls, 'rows':len(rows), 'first_pts':points[0],
              'last_pts':points[-1], 'pcm_bytes':len(payload), 'pcm_sha256':hashlib.sha256(payload).hexdigest(),
              'server_exit':process.returncode, 'client_exit':peer.returncode,
              'client_shutdown_seconds':shutdown_seconds,
              'server_argv':server, 'client_argv':client}
    (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def run_installed_pair(options):
    """One short shared encoder/two decoder run; no asset path fallback."""
    directory = options.evidence
    gate = directory / 'capture-ready'
    expected = options.seconds * 25
    calibration = options.only == 'installed-calibration6'
    received = [directory / f'healthy-{name}.{"json" if calibration else "pcm"}' for name in ('a', 'b')]
    dumps = [directory / f'healthy-{name}.out' for name in ('a', 'b')]
    errors = [directory / f'healthy-{name}.err' for name in ('a', 'b')]
    process = None
    peers = []
    commands = []
    row_pattern = r'KMX_AUDIO (\d+) #\d+ at=(\d+) epoch=(\d+) flags=(\d+)'
    marker = 'KMX_AUDIO_CODEC encodec-24k-mono-v1 bitrate=6 threads=2 profile=C5-R4'
    with tempfile.TemporaryDirectory(prefix='kmxe-pair-', dir='/tmp') as temporary:
        endpoint = Path(temporary) / 'session.sock'
        command = shlex.join([sys.executable, str(Path(__file__).resolve()), '--capture',
                              '24000', '1', str(gate), str(options.seconds + (100 if calibration else 0))])
        server = [str(options.new_bin / 'kmx-serve'), '--socket', str(endpoint),
                  '--pane', f'exec sleep {options.seconds + 60}', '--audio-source', 'exec ' + command,
                  '--audio-rate', '24000', '--audio-channels', '1', '--audio-bitrate', '6',
                  '--audio-codec', 'encodec']
        files = [(directory / 'serve.out').open('wb'), (directory / 'serve.err').open('wb')]
        try:
            process = subprocess.Popen(server, stdout=files[0], stderr=files[1], start_new_session=True)
            wait_for(endpoint.exists, process, 30)
            for index in range(2):
                client = [str(options.new_bin / 'kmx-attach'), '--socket', str(endpoint),
                          '--view', '--dump', '--reconnect', '0', '--audio-bitrate', '6',
                          '--audio-codec', 'encodec', '--audio-output',
                          ('exec ' + shlex.join([sys.executable, str(Path(__file__).resolve()), '--sink', str(received[index])])
                           if calibration else 'exec cat > ' + shlex.quote(str(received[index])))]
                out, err = dumps[index].open('wb'), errors[index].open('wb')
                files.extend((out, err))
                peers.append(subprocess.Popen(client, stdout=out, stderr=err, start_new_session=True))
                commands.append(client)

            def both_ready():
                assert all(peer.poll() is None for peer in peers), 'client ended before readiness'
                return all(marker in path.read_text() for path in dumps)

            wait_for(both_ready, process, 30)
            gate.touch()

            def both_complete():
                assert all(peer.poll() is None for peer in peers), 'client ended during transport'
                return all(len(re.findall(row_pattern, path.read_text())) >= expected for path in dumps)

            if calibration:
                measured = measure_calibration(options, process, peers, dumps)
            else:
                wait_for(both_complete, process, options.seconds + 10)
        finally:
            for peer in peers:
                stop(peer)
            stop(process)
            for file in files:
                file.close()
    assert process.returncode == 0 and all(peer.returncode == 0 for peer in peers)
    if calibration:
        result = finish_calibration(options, measured, received, dumps, errors)
        result.update({'server_argv': server, 'client_argv': commands})
        (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'passed': True, 'clients': 2, 'seconds': options.seconds,
                          'window_frames': [client['window_frames'] for client in result['clients']]}))
        return
    rows = []
    for index in range(2):
        records = [tuple(map(int, record)) for record in re.findall(row_pattern, dumps[index].read_text())]
        assert len(records) == expected, (index, len(records), expected)
        assert all(size == 1920 for size, _pts, _epoch, _flags in records)
        assert [record[1] for record in records] == [i * 40 for i in range(expected)], index
        assert records[0][3] == 13, records[0]
        pcm = received[index].read_bytes()
        assert len(pcm) == expected * 1920, (index, len(pcm))
        rows.append({'client': index, 'frames': len(records), 'pcm_bytes': len(pcm),
                     'pcm_sha256': hashlib.sha256(pcm).hexdigest()})
    # Check every profile and aggregate row; an unused clean C0 row alone
    # cannot hide losses in the selected C5-R4 stream.
    for path in [directory / 'serve.err', *errors]:
        stats = re.findall(r'kmx-(?:serve|attach): encodec(?:_profile)? calls=\d+ rtf=\S+ '
                           r'input_drops=(\d+) output_drops=(\d+) discontinuities=(\d+)', path.read_text())
        assert len(stats) == 3 and all(int(i) == 0 and int(o) == 0 for i, o, _d in stats), (path, stats)
        assert [int(row[2]) for row in stats] == [0, 1, 1], (path, stats)
    server_text = (directory / 'serve.err').read_text()
    assert re.findall(r'client_drops=(\d+)', server_text) and all(
        int(value) == 0 for value in re.findall(r'client_drops=(\d+)', server_text))
    result = {'passed': True, 'seconds': options.seconds, 'rows': rows,
              'server_argv': server, 'client_argv': commands,
              'scope': 'short installed two-client C5-R4 transport; not a calibration or soak'}
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'clients': 2, 'frames_each': expected, 'seconds': options.seconds}))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--sink':
        return sink(sys.argv[2])
    if len(sys.argv) in (5, 6) and sys.argv[1] == '--capture':
        seconds = int(sys.argv[5]) if len(sys.argv) == 6 else 2
        if not 1 <= seconds <= 1900:
            return 2
        return capture(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], seconds)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--new-bin',type=Path,required=True)
    parser.add_argument('--old-bin',type=Path)
    parser.add_argument('--assets',type=Path)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--only',choices=['unix6','all','compat','installed-pair6','installed-calibration6'],default='all')
    parser.add_argument('--seconds',type=int,choices=range(1,1801),default=30)
    parser.add_argument('--cgroup-unit',help='new caller-owned systemd scope used for calibration memory accounting')
    options=parser.parse_args()
    if not options.only.startswith('installed-') and options.assets is None:
        parser.error('--assets is required for the development matrix')
    if options.only.startswith('installed-') and options.assets is not None:
        parser.error('installed modes refuse --assets; provide installed Content and receipts')
    if options.only == 'installed-calibration6' and (options.seconds != 1800 or not options.cgroup_unit):
        parser.error('installed-calibration6 requires --seconds 1800 and a new --cgroup-unit')
    options.evidence.mkdir(parents=True,exist_ok=False)
    if options.only.startswith('installed-'):
        try:
            return run_installed_pair(options)
        except Exception as error:
            (options.evidence / 'failure.json').write_text(json.dumps(
                {'passed': False, 'error': str(error), 'type': type(error).__name__}, indent=2) + '\n')
            raise
    rows=[] if options.only=='compat' else [run_case(options,'unix6',6,24000,1)]
    if options.only=='all':
        rows += [run_case(options,'unix3-resample44100',3,44100,2),
                 run_case(options,'unix12-resample48000',12,48000,2)]
    if options.only in ('all','compat'):
        rows += [run_case(options,'tls6',6,48000,2,tls=True)]
        if options.old_bin:
            rows += [run_case(options,'legacy-server',6,48000,2,legacy_server=True),
                     run_case(options,'legacy-client',6,48000,2,legacy_client=True)]
    (options.evidence/'results.json').write_text(json.dumps({'all_passed':True,'rows':rows},indent=2)+'\n')
    print(json.dumps({'passed_cases':len(rows),'audio_rows':sum(row['rows'] for row in rows)}))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
