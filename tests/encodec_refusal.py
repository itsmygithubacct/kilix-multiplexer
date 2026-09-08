"""Real negotiation gates: refused offers receive no audio after capture starts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def frame(kind, body):
    length = len(body) + 1
    prefix = bytearray()
    while length >= 128:
        prefix.append((length & 127) | 128)
        length >>= 7
    return bytes(prefix) + bytes([length, kind]) + body


class Reader:
    def __init__(self, peer):
        self.peer = peer
        self.pending = bytearray()

    def collect(self, duration):
        rows = []
        deadline = time.monotonic() + duration
        self.peer.settimeout(.02)
        while time.monotonic() < deadline:
            try:
                part = self.peer.recv(65536)
            except TimeoutError:
                continue
            if not part:
                raise AssertionError('server closed the other negotiated planes')
            self.pending.extend(part)
            while self.pending:
                length = 0
                for offset, byte in enumerate(self.pending):
                    assert offset < 10
                    length |= (byte & 127) << (offset * 7)
                    if byte < 128:
                        break
                else:
                    break
                start = offset + 1
                assert 0 < length <= 8 * 1024 * 1024
                if len(self.pending) < start + length:
                    break
                rows.append((self.pending[start], bytes(self.pending[start+1:start+length])))
                del self.pending[:start+length]
        return rows


def capture(gate):
    deadline = time.monotonic() + 5
    while not gate.exists():
        if time.monotonic() > deadline:
            return 1
        time.sleep(.005)
    for _ in range(16):
        os.write(1, b'\0' * 960)
        time.sleep(.02)
    return 0


def run(binary, evidence):
    evidence.mkdir(exist_ok=False, parents=True)
    cases = [('legacy', None, True), ('pcm', 1, True), ('auto', 3, True),
             ('pcm-unknown', 129, True), ('encodec-only', 2, False),
             ('unknown-only', 128, False), ('empty', 0, False), ('encodec-unknown', 130, False)]
    rows = []
    for name, codecs, allowed in cases:
        directory = evidence / name
        directory.mkdir()
        gate = directory / 'gate'
        with tempfile.TemporaryDirectory(prefix='kmx-refuse-', dir='/tmp') as temp:
            endpoint = Path(temp) / 's'
            token = 'synthetic-private-fixture-token'
            command = [str(binary), '--socket', str(endpoint), '--pane', 'exec sleep 30',
                       '--token', token, '--audio-rate', '24000', '--audio-channels', '1',
                       '--audio-source', 'exec ' + shlex.join([sys.executable, str(Path(__file__).resolve()),
                                                            '--capture', str(gate)])]
            environment = os.environ.copy()
            environment['KILIX_CONTENT_ROOT'] = str(Path(temp) / 'uninstalled')
            with (directory/'stdout').open('wb') as output, (directory/'stderr').open('wb') as error:
                process = subprocess.Popen(command, env=environment, stdout=output, stderr=error, start_new_session=True)
                peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    deadline = time.monotonic() + 3
                    while not endpoint.exists():
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(.01)
                    peer.connect(str(endpoint))
                    peer.sendall(frame(1, struct.pack('>HHB', 24, 80, 1) + token.encode()))
                    if codecs is not None:
                        offer = b'KAC1' + bytes([0, codecs, 2, 0]) + struct.pack('>H', 160) + b'\0\0'
                        peer.sendall(frame(12, offer))
                    reader = Reader(peer)
                    initial = reader.collect(.1)
                    deadline = time.monotonic() + 3
                    while codecs is not None and not any(kind == 12 for kind, _ in initial):
                        assert time.monotonic() < deadline, 'capability response deadline'
                        initial.extend(reader.collect(.05))
                    caps = [body for kind, body in initial if kind == 12]
                    if codecs is not None:
                        expected = b'KAC1' + (bytes([1, 1, 0, 0]) if allowed else bytes([2, 0, 0, 0])) + b'\0'*4
                        assert caps == [expected], (name, caps)
                    else:
                        assert not caps
                    assert not any(kind == 11 for kind, _ in initial)
                    gate.touch()
                    later = reader.collect(.4)
                    audio = [body for kind, body in later if kind == 11]
                    rows.append(dict(case=name, allowed=allowed, audio_rows=len(audio),
                                     passed=(bool(audio) if allowed else not audio),
                                     caps=[body.hex() for body in caps], command=command))
                finally:
                    peer.close()
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=3)
    result = dict(all_passed=all(row['passed'] for row in rows), rows=rows,
                  server_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    (evidence/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'all_passed':result['all_passed'], 'cases':len(rows),
                      'failed':[row['case'] for row in rows if not row['passed']]}))
    return 0 if result['all_passed'] else 1


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--capture':
        raise SystemExit(capture(Path(sys.argv[2])))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    options = parser.parse_args()
    raise SystemExit(run(options.serve.resolve(), options.evidence.resolve()))
