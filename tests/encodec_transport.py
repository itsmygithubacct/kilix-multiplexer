"""Real optional EnCodec transport controls with synthetic PCM and local graphs.

This exercises the explicit development path. It does not grant installed
admission, wire performance, listening or hardware-profile qualification.
"""
from __future__ import annotations

import argparse
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


def capture(rate, channels, gate):
    until = time.monotonic() + 30
    while not Path(gate).exists():
        if time.monotonic() > until:
            return 1
        time.sleep(.01)
    started = time.monotonic()
    for packet in range(100):
        samples = []
        for offset in range(rate // 50):
            index = packet * (rate // 50) + offset
            value = round(6000 * math.sin(index * 2 * math.pi * 440 / rate))
            for channel in range(channels):
                samples.append(value if channel % 2 == 0 else value // 2)
        sys.stdout.buffer.write(struct.pack('<' + 'h' * len(samples), *samples))
        sys.stdout.buffer.flush()
        time.sleep(max(0, started + (packet + 1) * .02 - time.monotonic()))
    return 0


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


def main():
    if len(sys.argv) == 5 and sys.argv[1] == '--capture':
        return capture(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--new-bin',type=Path,required=True)
    parser.add_argument('--old-bin',type=Path)
    parser.add_argument('--assets',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--only',choices=['unix6','all','compat'],default='all')
    options=parser.parse_args()
    options.evidence.mkdir(parents=True,exist_ok=False)
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
