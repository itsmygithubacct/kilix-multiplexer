"""Audio received before the EnCodec selection is discarded, not fatal.

A server in auto mode sends legacy PCM to every greeted peer whose AUDIO_CAPS
offer it has not yet read. Over TLS the HELLO and the offer can arrive in
separate reads, so one block can precede the selection. This fake server forces
that order deterministically: HELLO, then an AUDIO frame, then the selection.
After the selection the strict refusal still applies, so a malformed EnCodec
frame must still end the session with exit 1.

Omit --assets to exercise genuine installed admission. Exact receipt and
Content configuration must already be supplied by the caller.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from encodec_transport import stop, wait_for
from kmxwire import frame


class Reader:
    def __init__(self, peer):
        self.peer = peer
        self.pending = bytearray()

    def next(self, deadline):
        self.peer.settimeout(.05)
        while time.monotonic() < deadline:
            if self.pending:
                size = 0
                for offset, byte in enumerate(self.pending):
                    assert offset < 10
                    size |= (byte & 127) << (offset * 7)
                    if byte < 128:
                        break
                else:
                    offset = None
                if offset is not None and len(self.pending) >= offset + 1 + size:
                    start = offset + 1
                    message = (self.pending[start], bytes(self.pending[start + 1:start + size]))
                    del self.pending[:start + size]
                    return message
            try:
                part = self.peer.recv(65536)
            except TimeoutError:
                continue
            assert part, 'attach closed the connection'
            self.pending.extend(part)
        raise AssertionError('message deadline')


def case(options, root, name, preselection_frames):
    directory = root / name
    directory.mkdir()
    with tempfile.TemporaryDirectory(prefix='kmx-presel-', dir='/tmp') as temporary:
        endpoint = Path(temporary) / 'session'
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(endpoint)); listener.listen(1)
        command = [str(options.attach), '--socket', str(endpoint), '--view', '--dump', '--reconnect', '0',
                   '--audio-codec', 'encodec', '--audio-threads', '4', '--audio-output', 'exec cat > /dev/null']
        if options.assets:
            command += ['--development-encodec-assets', str(options.assets)]
        out = (directory / 'attach.out').open('wb'); err = (directory / 'attach.err').open('wb')
        process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        try:
            listener.settimeout(30)
            peer, _ = listener.accept()
            reader = Reader(peer)
            deadline = time.monotonic() + 10
            kind, _ = reader.next(deadline)
            assert kind == 1, ('first message is not HELLO', kind)
            # The race, made deterministic: audio between HELLO and selection.
            for body in preselection_frames:
                peer.sendall(frame(11, body))
            offer = None
            while offer is None:
                kind, body = reader.next(deadline)
                if kind == 12:
                    offer = body
            assert offer[:4] == b'KAC1' and offer[4] == 0, offer
            assert offer[5] & 2, ('attach did not offer EnCodec', offer)
            # Select EnCodec at the offered rate; no profile marker selects C0.
            peer.sendall(frame(12, b'KAC1' + bytes([1, 2, offer[6], 0]) + offer[8:10] + b'\0\0'))
            wait_for(lambda: b'KMX_AUDIO_CODEC encodec-24k-mono-v1' in (directory / 'attach.out').read_bytes(),
                     process, 10)
            time.sleep(.5)
            assert process.poll() is None, ('attach exited after selection', process.returncode)
            # Strictness after selection is unchanged: a malformed EnCodec frame is fatal.
            peer.sendall(frame(11, b'BAD!'))
            process.wait(timeout=10)
            peer.close()
        finally:
            stop(process); out.close(); err.close(); listener.close()
    stderr = (directory / 'attach.err').read_text(errors='replace')
    assert process.returncode == 1, ('malformed post-selection frame was not refused', process.returncode)
    expected = len(preselection_frames)
    if expected:
        assert f'discarded {expected} audio block(s) received before the EnCodec selection' in stderr, stderr
    else:
        assert 'received before the EnCodec selection' not in stderr, stderr
    return dict(case=name, passed=True, preselection_frames=expected, returncode=process.returncode)


def run(options):
    options.evidence.mkdir(parents=True, exist_ok=False)
    rows = [case(options, options.evidence, 'none', []),
            case(options, options.evidence, 'one-pcm-block', [b'\0' * 64]),
            case(options, options.evidence, 'three-blocks', [b'\0' * 64, b'KMA\2' + b'\0' * 12, b'x'])]
    result = dict(rows=rows, passed=True,
                  attach_sha256=hashlib.sha256(options.attach.read_bytes()).hexdigest(),
                  scope='bounded pre-selection audio handling only; no audio, timing or release qualification')
    (options.evidence / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'cases': len(rows)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attach', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--assets', type=Path)
    run(parser.parse_args())
