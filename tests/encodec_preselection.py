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


def handshake(reader, peer, deadline, frames):
    kind, _ = reader.next(deadline)
    assert kind == 1, ('first message is not HELLO', kind)
    # The race, made deterministic: audio between HELLO and selection.
    for body in frames:
        peer.sendall(frame(11, body))
    while True:
        kind, body = reader.next(deadline)
        if kind == 12:
            assert body[:4] == b'KAC1' and body[4] == 0, body
            assert body[5] & 2, ('attach did not offer EnCodec', body)
            return body


def select(peer, offer):
    # Select EnCodec at the offered rate; no profile marker selects C0.
    peer.sendall(frame(12, b'KAC1' + bytes([1, 2, offer[6], 0]) + offer[8:10] + b'\0\0'))


def case(options, root, name, preselection_frames, mode='encodec', connections=1):
    """Each connection: HELLO, the given AUDIO frames, then the selection.
    After the last selection a malformed EnCodec frame must still be fatal."""
    directory = root / name
    directory.mkdir()
    codec_line = b'KMX_AUDIO_CODEC encodec-24k-mono-v1'
    with tempfile.TemporaryDirectory(prefix='kmx-presel-', dir='/tmp') as temporary:
        endpoint = Path(temporary) / 'session'
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(endpoint)); listener.listen(1)
        command = [str(options.attach), '--socket', str(endpoint), '--view', '--dump',
                   '--reconnect', '10' if connections > 1 else '0',
                   '--audio-codec', mode, '--audio-threads', '4', '--audio-output', 'exec cat > /dev/null']
        if options.assets:
            command += ['--development-encodec-assets', str(options.assets)]
        out = (directory / 'attach.out').open('wb'); err = (directory / 'attach.err').open('wb')
        process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        try:
            listener.settimeout(30)
            for connection in range(connections):
                peer, _ = listener.accept()
                reader = Reader(peer)
                offer = handshake(reader, peer, time.monotonic() + 10, preselection_frames)
                select(peer, offer)
                wait_for(lambda: (directory / 'attach.out').read_bytes().count(codec_line) > connection,
                         process, 10)
                time.sleep(.5)
                assert process.poll() is None, ('attach exited after selection', process.returncode)
                if connection + 1 < connections:
                    peer.close()           # a dropped link; the attach reconnects and renegotiates
            # Strictness after selection is unchanged: a malformed EnCodec frame is fatal.
            peer.sendall(frame(11, b'BAD!'))
            process.wait(timeout=10)
            peer.close()
        finally:
            stop(process); out.close(); err.close(); listener.close()
    stderr = (directory / 'attach.err').read_text(errors='replace')
    assert process.returncode == 1, ('malformed post-selection frame was not refused', process.returncode)
    expected = len(preselection_frames) * connections if mode == 'encodec' else 0
    if expected:
        assert f'discarded {expected} audio block(s) received before the EnCodec selection' in stderr, stderr
    else:
        # auto mode offers PCM too, so pre-selection audio goes to the PCM sink as before.
        assert 'received before the EnCodec selection' not in stderr, stderr
    return dict(case=name, passed=True, mode=mode, connections=connections,
                preselection_frames=len(preselection_frames), discarded=expected, returncode=process.returncode)


def never_selected(options, root):
    """Discarded audio must not extend the wait: with no selection the attach
    still gives up at its selection deadline, with its existing message."""
    directory = root / 'never-selected'
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
            kind, _ = reader.next(time.monotonic() + 10)
            assert kind == 1, ('first message is not HELLO', kind)
            greeted = time.monotonic(); sent = 0
            while process.poll() is None and time.monotonic() - greeted < 8:
                try:
                    peer.sendall(frame(11, b'\0' * 64)); sent += 1
                except (BrokenPipeError, ConnectionResetError):
                    break
                time.sleep(.05)
            process.wait(timeout=5)
            elapsed = time.monotonic() - greeted
            peer.close()
        finally:
            stop(process); out.close(); err.close(); listener.close()
    stderr = (directory / 'attach.err').read_text(errors='replace')
    assert process.returncode == 1, process.returncode
    assert 'peer did not select the requested EnCodec profile' in stderr, stderr
    assert 1.5 <= elapsed <= 4.0, ('selection deadline not honoured', elapsed)
    assert 'received before the EnCodec selection' in stderr, stderr
    assert b'KMX_AUDIO_CODEC' not in (directory / 'attach.out').read_bytes()
    return dict(case='never-selected', passed=True, frames_sent=sent, exit_after_hello_s=round(elapsed, 3),
                returncode=process.returncode)


def run(options):
    options.evidence.mkdir(parents=True, exist_ok=False)
    root = options.evidence
    rows = [case(options, root, 'none', []),
            case(options, root, 'one-pcm-block', [b'\0' * 64]),
            case(options, root, 'three-blocks', [b'\0' * 64, b'KMA\2' + b'\0' * 12, b'x']),
            case(options, root, 'reconnect-renegotiates', [b'\0' * 64], connections=2),
            case(options, root, 'auto-mode-unchanged', [b'\0' * 64], mode='auto'),
            never_selected(options, root)]
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
