"""Bounded real profile-handshake controls; no audio/timing qualification.

Omit --assets to exercise genuine installed admission. Exact receipt and
Content configuration must already be supplied by the caller.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from encodec_transport import stop, wait_for
from kmxwire import frame, hello


def profile(kind, value):
    return b'KEP1' + bytes([kind, 0, 0, 0]) + struct.pack('>I', value)


def caps(codecs=2):
    return b'KAC1' + bytes([0, codecs, 2, 0]) + struct.pack('>H', 160) + b'\0\0'


def collect(peer, wanted, closed=False):
    pending = bytearray()
    rows = []
    deadline = time.monotonic() + 2
    peer.settimeout(.05)
    while time.monotonic() < deadline:
        try:
            part = peer.recv(65536)
        except TimeoutError:
            continue
        if not part:
            assert closed, 'unexpected connection close'
            return rows
        pending.extend(part)
        while pending:
            size = 0
            for offset, byte in enumerate(pending):
                assert offset < 10
                size |= (byte & 127) << (offset * 7)
                if byte < 128:
                    break
            else:
                break
            start = offset + 1
            assert 0 < size <= 8 * 1024 * 1024
            if len(pending) < start + size:
                break
            rows.append((pending[start], bytes(pending[start+1:start+size])))
            del pending[:start+size]
        if not closed and sum(kind in (12, 13) for kind, _ in rows) >= wanted:
            return rows
    raise AssertionError('profile exchange deadline')


def run(options):
    options.evidence.mkdir(parents=True, exist_ok=False)
    token = 'synthetic-profile-control-token'
    rows = []
    with tempfile.TemporaryDirectory(prefix='kmx-prof-', dir='/tmp') as temporary:
        endpoint = Path(temporary) / 'session'
        command = [str(options.serve), '--socket', str(endpoint), '--pane', 'exec sleep 30',
                   '--audio-source', 'exec sleep 30', '--audio-rate', '24000',
                   '--audio-channels', '1', '--audio-threads', '4', '--token', token]
        if options.assets:
            command += ['--development-encodec-assets', str(options.assets)]
        with (options.evidence/'serve.out').open('wb') as out, (options.evidence/'serve.err').open('wb') as err:
            process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
            try:
                wait_for(lambda: endpoint.exists(), process)
                wait_for(lambda: 'profiles=C0,C5-R4' in (options.evidence/'serve.err').read_text(), process)
                positives = [('both', [profile(0, 3)], 2, 1),
                             ('c0', [profile(0, 1)], 2, 0),
                             ('unknown-offer-bits', [profile(0, 0x80000003)], 2, 1),
                             ('absent', [], 2, None),
                             ('identical-offers', [profile(0, 3)] * 2, 2, 1),
                             ('pcm', [profile(0, 3)], 1, None)]
                for name, offers, codecs, selected in positives:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                        peer.connect(str(endpoint))
                        peer.sendall(hello(role=1, token=token) + b''.join(frame(13, p) for p in offers) + frame(12, caps(codecs)))
                        messages = collect(peer, 2 if selected is not None else 1)
                        choices = [(kind, body) for kind, body in messages if kind in (12, 13)]
                        expected = ([(13, profile(1, selected))] if selected is not None else [])
                        expected += [(12, b'KAC1' + bytes([1, codecs, 2 if codecs == 2 else 0, 0]) +
                                      (struct.pack('>H', 160) if codecs == 2 else b'\0\0') + b'\0\0')]
                        assert choices == expected, (name, choices)
                        peer.sendall(frame(12, caps(codecs)))
                        duplicates = [(kind, body) for kind, body in collect(peer, len(expected)) if kind in (12, 13)]
                        assert duplicates == expected, (name, duplicates)
                        rows.append(dict(case=name, passed=True, selected_profile=selected, duplicate_unchanged=True))
                invalid = [('bad-magic', b'JEP1' + profile(0, 3)[4:]),
                           ('short', profile(0, 3)[:-1]), ('long', profile(0, 3) + b'\0'),
                           ('reserved', profile(0, 3)[:5] + b'\1' + profile(0, 3)[6:]),
                           ('selection-as-offer', profile(1, 1)),
                           ('missing-c0', profile(0, 2)), ('bad-kind', profile(2, 3))]
                for name, body in invalid:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                        peer.connect(str(endpoint)); peer.sendall(hello(role=1, token=token) + frame(13, body))
                        collect(peer, 0, closed=True)
                        rows.append(dict(case=name, passed=True, refused=True))
                for name, messages in [('changed-offer', frame(13, profile(0, 3)) + frame(13, profile(0, 1))),
                                       ('after-selection', frame(12, caps()) + frame(13, profile(0, 3))),
                                       ('pre-hello', frame(13, profile(0, 3)))]:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                        peer.connect(str(endpoint))
                        peer.sendall((b'' if name == 'pre-hello' else hello(role=1, token=token)) + messages)
                        collect(peer, 0, closed=True)
                        rows.append(dict(case=name, passed=True, refused=True))
            finally:
                stop(process)
    assert process.returncode == 0, process.returncode
    result = dict(rows=rows, passed=True, command=command,
                  serve_sha256=hashlib.sha256(options.serve.read_bytes()).hexdigest(),
                  server_returncode=process.returncode,
                  scope='bounded negotiation controls only; no audio, concurrency or release qualification')
    (options.evidence/'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed':True, 'cases':len(rows)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--assets', type=Path)
    run(parser.parse_args())
