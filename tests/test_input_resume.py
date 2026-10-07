#!/usr/bin/env python3
"""Owned PTY/socket tests for same-server, memory-only input resume.

python3 tests/test_input_resume.py --server build/kmx-serve \
    --attach build/kmx-attach --library build/libkilix-mux.so
Add --expiry to exercise the real 60-second disconnected retention deadline.
ACK proves bounded server FIFO acceptance, not application execution.
"""
import argparse
import json
import os
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

from test_attach_modes import (Attach, RESET, frame, layout_message,
    load_terminal_library, next_frame, stop_owned_child, stop_process, wait_path)

OPEN, STATE, DATA, ACK, CLOSE = 15, 16, 17, 18, 19
ZERO = bytes(16)
KEY = b"owned-client-key"
OTHER = b"other-client-key"
FAKE_EPOCH = b"fake-server-epoc"
TOKEN = b"0123456789abcdef0123456789abcdef"
assert len(KEY) == len(OTHER) == len(FAKE_EPOCH) == 16

CHILD = r'''
import fcntl,json,os,select,struct,sys,termios,time,tty
from pathlib import Path
result,pid,ready,dimensions=sys.argv[1:]
Path(pid).write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
tty.setraw(0); Path(ready).write_text("ready")
received=b""; deadline=time.monotonic()+100
while time.monotonic()<deadline:
    size=struct.unpack("HHHH",fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2]
    temporary=Path(dimensions+".tmp"); temporary.write_text(json.dumps(size)); temporary.replace(dimensions)
    if not select.select([0],[],[],.02)[0]: continue
    data=os.read(0,65536)
    if not data: break
    received+=data
    temporary=Path(result+".tmp"); temporary.write_bytes(received); temporary.replace(result)
'''


def opening(epoch=ZERO, key=KEY, acknowledged=0):
    return b"\x01\0\0\0" + epoch + key + struct.pack("!Q", acknowledged)


def state(epoch, key, accepted=0, status=0):
    return bytes([1, status, 0, 0]) + epoch + key + struct.pack("!QI", accepted, 60000)


def data(sequence, payload, pane=0):
    return bytes([1, pane, 0, 0]) + struct.pack("!Q", sequence) + payload


def ack(accepted):
    return b"\x01\0\0\0" + struct.pack("!Q", accepted)


def decode_state(payload, status=0, key=KEY):
    assert len(payload) == 48 and payload[:4] == bytes([1, status, 0, 0]), payload
    assert payload[4:20] != ZERO and payload[20:36] == key, payload
    accepted, grace = struct.unpack("!QI", payload[36:])
    assert grace == 60000, grace
    if status:
        assert accepted == 0, "error STATE exposed another owner's watermark"
    return payload[4:20], accepted


class Peer:
    def __init__(self, connection, client=None):
        self.socket = connection
        self.socket.setblocking(False)
        self.client = client
        self.pending = bytearray()
        self.seen = []
        self.eof = False

    def send(self, kind, payload=b""):
        self.socket.setblocking(True)
        self.socket.settimeout(2)
        try:
            self.socket.sendall(frame(kind, payload))
        finally:
            self.socket.setblocking(False)

    def pump(self, timeout=.01):
        if self.client:
            self.client.pump(0)
        if self.eof or not select.select([self.socket], [], [], timeout)[0]:
            return
        try:
            chunk = self.socket.recv(65536)
        except ConnectionResetError:
            chunk = b""
        if not chunk:
            self.eof = True
            return
        self.pending.extend(chunk)
        while True:
            message = next_frame(self.pending)
            if message is None:
                break
            kind, payload, consumed = message
            del self.pending[:consumed]
            self.seen.append((kind, bytes(payload)))

    def wait(self, kind, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, (received, payload) in enumerate(self.seen):
                if received == kind:
                    del self.seen[index]
                    return payload
            assert not self.eof, f"peer closed before frame {kind}; frames={self.seen}"
            self.pump()
        raise AssertionError(f"missing frame {kind}; frames={self.seen}")

    def closed(self, timeout=3):
        deadline = time.monotonic() + timeout
        while not self.eof and time.monotonic() < deadline:
            self.pump()
        assert self.eof, "peer did not fence/close the transport"

    def close(self):
        self.socket.close()


class OwnedServer:
    def __init__(self, binary, folder, panes=1):
        self.folder = folder
        self.folder.mkdir()
        self.endpoint = folder / "server.sock"
        self.paths = []
        commands = []
        for pane in range(panes):
            paths = tuple(folder / f"{name}-{pane}" for name in ("bytes", "pid", "ready", "size"))
            self.paths.append(paths)
            command = [sys.executable, "-c", CHILD, *map(str, paths)]
            commands += ["--pane", shlex.join(command)] if panes > 1 else ["--", *command]
        self.log = tempfile.TemporaryFile(dir=folder)
        self.process = subprocess.Popen([str(binary), "--socket", str(self.endpoint),
            "--token", TOKEN.decode(), "--rows", "12", "--cols", "60", *commands], stdout=self.log, stderr=self.log)
        self.peers = []
        self.next_connect = 0.0
        try:
            for paths in self.paths:
                wait_path(paths[2], self.process)
            wait_path(self.endpoint, self.process)
        except BaseException:
            self.log.seek(0)
            diagnostic = self.log.read().decode(errors="replace")
            self.close()
            raise AssertionError(f"owned server failed readiness: {diagnostic}")

    def peer(self, role=0, rows=12, cols=60):
        # Exercise protocol errors without turning this many short-lived
        # connections into a test of the server's accept-rate limiter.
        delay = self.next_connect - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        self.next_connect = time.monotonic() + .13
        connection = socket.socket(socket.AF_UNIX)
        connection.connect(str(self.endpoint))
        peer = Peer(connection)
        self.peers.append(peer)
        peer.send(1, struct.pack("!HHB", rows, cols, role) + TOKEN)
        peer.wait(7)
        return peer

    def received(self):
        path = self.paths[0][0]
        return path.read_bytes() if path.exists() else b""

    def expect(self, wanted, timeout=3):
        deadline = time.monotonic() + timeout
        while self.received() != wanted and time.monotonic() < deadline:
            assert self.process.poll() is None, "PTY server exited"
            time.sleep(.01)
        assert self.received() == wanted, (self.received(), wanted)

    def close(self):
        for peer in self.peers:
            peer.close()
        for paths in self.paths:
            stop_owned_child(paths[1])
        stop_process(self.process)
        self.log.close()


def server_protocol(binary, folder, expiry):
    server = OwnedServer(binary, folder / "server")
    try:
        stranger = socket.socket(socket.AF_UNIX)
        stranger.connect(str(server.endpoint))
        unauthenticated = Peer(stranger)
        server.peers.append(unauthenticated)
        unauthenticated.send(OPEN, opening())
        unauthenticated.closed()
        owner = server.peer()
        owner.send(OPEN, opening())
        epoch, accepted = decode_state(owner.wait(STATE))
        assert accepted == 0
        owner.send(DATA, data(1, b"first"))
        # Deliberately do not consume ACK: child observation proves this test's
        # input was admitted before the connection was lost.
        server.expect(b"first")
        owner.close()
        owner = server.peer()
        owner.send(OPEN, opening(epoch))
        assert decode_state(owner.wait(STATE)) == (epoch, 1)
        owner.send(DATA, data(1, b"DUPLICATE-MUST-NOT-REACH-PTY"))
        assert owner.wait(ACK) == ack(1)
        owner.send(DATA, data(2, b"second"))
        assert owner.wait(ACK) == ack(2)
        server.expect(b"firstsecond")
        owner.send(DATA, data(4, b"GAP-MUST-NOT-REACH-PTY"))
        owner.closed()

        old = server.peer()
        old.send(OPEN, opening(epoch, acknowledged=1))
        assert decode_state(old.wait(STATE)) == (epoch, 2)
        owner = server.peer()
        owner.send(OPEN, opening(epoch, acknowledged=2))
        assert decode_state(owner.wait(STATE)) == (epoch, 2)
        old.closed()
        owner.send(DATA, data(3, b"third"))
        assert owner.wait(ACK) == ack(3)
        server.expect(b"firstsecondthird")

        viewer = server.peer(role=1)
        viewer.send(OPEN, opening(epoch))
        decode_state(viewer.wait(STATE), status=5)
        viewer.send(3, b"VIEWER-MUST-NOT-REACH-PTY")
        contender = server.peer()
        contender.send(OPEN, opening(key=OTHER))
        decode_state(contender.wait(STATE), status=1, key=OTHER)
        contender.close()
        # A tiny legacy HELLO must not resize a leased PTY. Check dimensions
        # after a later owner DATA roundtrip, not just immediately after HELLO.
        legacy = server.peer(rows=3, cols=9)
        owner.send(DATA, data(4, b"fourth"))
        assert owner.wait(ACK) == ack(4)
        server.expect(b"firstsecondthirdfourth")
        time.sleep(.05)
        assert json.loads(server.paths[0][3].read_text()) == [12, 60], "legacy HELLO resized leased PTY"
        legacy.send(4, struct.pack("!HH", 4, 10))
        legacy.closed()
        legacy = server.peer()
        legacy.send(3, b"LEGACY-MUST-NOT-REACH-PTY")
        legacy.closed()
        legacy = server.peer()
        legacy.send(8, b"\0")
        legacy.closed()

        # A stale CLOSE is a disconnect, not a release of accepted dedup state.
        owner.send(CLOSE, ack(3))
        owner.closed()
        contender = server.peer()
        contender.send(OPEN, opening(key=OTHER))
        decode_state(contender.wait(STATE), status=1, key=OTHER)
        contender.close()
        owner = server.peer()
        owner.send(OPEN, opening(epoch, acknowledged=4))
        assert decode_state(owner.wait(STATE)) == (epoch, 4)
        owner.send(CLOSE, ack(4))
        owner.closed()
        expired = server.peer()
        expired.send(OPEN, opening(epoch, acknowledged=4))
        decode_state(expired.wait(STATE), status=2)
        expired.close()
        owner = server.peer()
        owner.send(OPEN, opening())
        retired_epoch = epoch
        epoch, accepted = decode_state(owner.wait(STATE))
        assert accepted == 0 and epoch != retired_epoch, "retired key recreated its old ledger epoch"
        stale = server.peer()
        stale.send(OPEN, opening(retired_epoch, acknowledged=0))
        assert decode_state(stale.wait(STATE), status=3)[0] == epoch
        stale.close()
        owner.send(CLOSE, ack(0))
        owner.closed()
        owner = server.peer()
        owner.send(OPEN, opening(key=OTHER))
        previous_epoch = epoch
        epoch, accepted = decode_state(owner.wait(STATE), key=OTHER)
        assert accepted == 0 and epoch != previous_epoch
        owner.send(CLOSE, ack(0))
        owner.closed()
        time.sleep(.05)
        server.expect(b"firstsecondthirdfourth")

        legacy = server.peer()
        contender = server.peer()
        contender.send(OPEN, opening(key=OTHER))
        decode_state(contender.wait(STATE), status=1, key=OTHER)
        contender.close()
        legacy.close()
        # Release processing is ordered by a new owned protocol request.
        owner = server.peer()
        owner.send(OPEN, opening())
        previous_epoch = epoch
        epoch, accepted = decode_state(owner.wait(STATE))
        assert accepted == 0 and epoch != previous_epoch
        owner.send(CLOSE, ack(0))
        owner.closed()

        if expiry:
            owner = server.peer()
            owner.send(OPEN, opening())
            previous_epoch = epoch
            epoch, accepted = decode_state(owner.wait(STATE))
            assert accepted == 0 and epoch != previous_epoch
            owner.send(DATA, data(1, b"retained"))
            assert owner.wait(ACK) == ack(1)
            owner.close()
            time.sleep(60.2)
            expired = server.peer()
            expired.send(OPEN, opening(epoch, acknowledged=1))
            decode_state(expired.wait(STATE), status=2)
            expired.close()
            server.expect(b"firstsecondthirdfourthretained")
        print("PASS server: authenticated OPEN, lost ACK, duplicate/gap, live fencing, viewer/control lease, CLOSE" +
              (", 60-second expiry" if expiry else ""))
    finally:
        server.close()

    restarted = OwnedServer(binary, folder / "restarted")
    try:
        peer = restarted.peer()
        peer.send(OPEN, opening(epoch, acknowledged=4))
        new_epoch, _ = decode_state(peer.wait(STATE), status=3)
        assert new_epoch != epoch
        assert restarted.received() == b"", "old epoch replayed into restarted PTY"
        print("PASS server restart refuses old epoch")
    finally:
        restarted.close()
    multi = OwnedServer(binary, folder / "multi", panes=2)
    try:
        peer = multi.peer()
        peer.send(OPEN, opening())
        decode_state(peer.wait(STATE), status=5)
        print("PASS multi-pane reliable input refused")
    finally:
        multi.close()


def accept_peer(listener, client, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        client.pump(0)
        if select.select([listener], [], [], .01)[0]:
            connection, _ = listener.accept()
            peer = Peer(connection, client)
            greeting = peer.wait(1)
            assert len(greeting) >= 5 and greeting[4] == 0, greeting
            request = peer.wait(OPEN)
            assert len(request) == 44 and request[:4] == b"\x01\0\0\0", request
            assert request[20:36] != ZERO
            return peer, request
        assert client.process.poll() is None, client.logs()
    raise AssertionError("attach did not establish a reliable-input connection")


def ready(peer, request, accepted=0):
    peer.send(STATE, state(FAKE_EPOCH, request[20:36], accepted))
    # A real layout and terminal modes make this an interactive PTY test.
    peer.socket.setblocking(True)
    peer.socket.sendall(layout_message() + frame(14, struct.pack("!BBHI", 1, 0, 0, 3)))
    peer.socket.setblocking(False)
    peer.client.wait_flags(3)


def reliable_data(peer, sequence):
    payload = peer.wait(DATA)
    assert len(payload) > 12 and payload[:4] == b"\x01\0\0\0", payload
    assert struct.unpack("!Q", payload[4:12])[0] == sequence, payload
    assert not any(kind == 3 for kind, _ in peer.seen), "attach fell back to legacy INPUT"
    return payload[12:]


def client_fixture(binary, library, folder, name, function, reconnect=2, options=()):
    endpoint = folder / (name + ".sock")
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(endpoint))
    listener.listen(4)
    client = Attach(binary, endpoint, library, folder,
                    ["--reliable-input", "--reconnect", str(reconnect), *options])
    peers = []
    try:
        function(listener, client, peers)
    finally:
        for peer in peers:
            peer.close()
        client.close()
        listener.close()


def attach_resume(listener, client, peers, partial=False):
    peer, request = accept_peer(listener, client)
    peers.append(peer)
    assert request[4:20] == ZERO and request[36:] == bytes(8)
    ready(peer, request)
    payload = b"partial transport input" if partial else b"accepted before lost ACK"
    client.send(payload)
    if partial:
        # Consume a DATA frame header and only five payload-header bytes, then
        # abandon this parser. No complete DATA has been accepted by the peer.
        partial_frame = bytearray()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            client.pump(0)
            if not select.select([peer.socket], [], [], .01)[0]:
                continue
            partial_frame.extend(peer.socket.recv(1))
            # All earlier complete controls can precede DATA. Consume them.
            message = next_frame(partial_frame)
            if message:
                kind, _, consumed = message
                assert kind != DATA, "test unexpectedly consumed complete DATA"
                del partial_frame[:consumed]
            if len(partial_frame) >= 7 and partial_frame[1] == DATA:
                break
        assert len(partial_frame) >= 7 and partial_frame[1] == DATA, partial_frame
    else:
        assert reliable_data(peer, 1) == payload
    disconnected = time.monotonic()
    peer.close()
    resumed, retry = accept_peer(listener, client)
    peers.append(resumed)
    assert retry[4:20] == FAKE_EPOCH and retry[20:36] == request[20:36]
    assert retry[36:] == bytes(8), "lost ACK was falsely acknowledged"
    ready(resumed, retry, accepted=0 if partial else 1)
    replayed_bytes = 0
    if partial:
        replay = reliable_data(resumed, 1)
        assert replay == payload
        replayed_bytes = len(replay)
        resumed.send(ACK, ack(1))
    client.send(b"next")
    assert reliable_data(resumed, 2) == b"next", "accepted DATA was replayed or sequence skipped"
    recovery_ms = (time.monotonic() - disconnected) * 1000
    resumed.send(ACK, ack(2))
    # Ensure attach consumes the ACK before its explicit detach request.
    for _ in range(5):
        client.pump(.01)
    client.send(b"\x1d")
    assert resumed.wait(CLOSE) == ack(2), "graceful CLOSE lacked accepted watermark"
    client.wait(lambda: client.process.poll() is not None, "attach did not exit after CLOSE")
    assert client.process.returncode == 0, client.logs()
    assert client.terminal.flags == 0 and RESET in client.output
    assert termios.tcgetattr(client.slave) == client.original_termios
    print("PASS actual attach: " + ("interrupted DATA replay" if partial else "lost ACK dedup resume") +
          f", recovery={recovery_ms:.1f}ms accepted_bytes={len(payload) + 4} replayed_bytes={replayed_bytes}, graceful CLOSE and cleanup")


def attach_handshake_retry(listener, client, peers):
    peer, request = accept_peer(listener, client)
    peers.append(peer)
    peer.close()  # OPEN may have been accepted, but STATE was never delivered.
    resumed, retry = accept_peer(listener, client)
    peers.append(resumed)
    assert retry == request, "unconfirmed initial selection changed client identity"
    resumed.close()
    resumed, retry = accept_peer(listener, client)
    peers.append(resumed)
    assert retry == request
    ready(resumed, retry)
    client.send(b"after repeated OPEN")
    assert reliable_data(resumed, 1) == b"after repeated OPEN"
    resumed.send(ACK, ack(1))
    for _ in range(5):
        client.pump(.01)
    client.send(b"\x1d")
    assert resumed.wait(CLOSE) == ack(1)
    client.wait(lambda: client.process.poll() is not None, "handshake retry did not detach")
    assert client.process.returncode == 0, client.logs()
    print("PASS actual attach: repeated pre-STATE disconnect preserves identity")


def attach_send_chunks(listener, client, peers, text):
    peer, request = accept_peer(listener, client)
    peers.append(peer)
    # A --send command must wait for READY as keyboard input does.
    for _ in range(5):
        peer.pump()
    assert not any(kind in (3, DATA) for kind, _ in peer.seen), "--send bypassed OPEN selection"
    ready(peer, request)
    received = bytearray()
    sequence = 0
    while len(received) < len(text):
        sequence += 1
        chunk = reliable_data(peer, sequence)
        assert 1 <= len(chunk) <= 32768
        received.extend(chunk)
    assert bytes(received) == text.encode()
    assert sequence == (len(text) + 32767) // 32768
    peer.send(ACK, ack(sequence))
    for _ in range(5):
        client.pump(.01)
    client.send(b"\x1d")
    assert peer.wait(CLOSE) == ack(sequence)
    client.wait(lambda: client.process.poll() is not None, "--send did not detach")
    assert client.process.returncode == 0, client.logs()
    print(f"PASS actual attach: READY-gated --send {len(received)} bytes in {sequence} ordered bounded frames")


def attach_resume_refused(listener, client, peers, failure):
    peer, request = accept_peer(listener, client)
    peers.append(peer)
    ready(peer, request)
    client.send(b"retained")
    assert reliable_data(peer, 1) == b"retained"
    if failure == "resume-counter":
        peer.send(ACK, ack(1))
        for _ in range(5):
            client.pump(.01)
    peer.close()
    resumed, request = accept_peer(listener, client)
    peers.append(resumed)
    replacement_epoch = b"changed-epoch!!!" if failure == "resume-epoch" else FAKE_EPOCH
    assert len(replacement_epoch) == 16
    resumed.send(STATE, state(replacement_epoch, request[20:36], accepted=0))
    deadline = time.monotonic() + 3
    while client.process.poll() is None and time.monotonic() < deadline:
        resumed.pump()
    assert client.process.poll() is not None and client.process.returncode != 0, client.logs()
    assert not any(kind in (3, DATA, CLOSE) for kind, _ in resumed.seen), "inconsistent STATE triggered replay/close"
    assert client.terminal.flags == 0 and termios.tcgetattr(client.slave) == client.original_termios
    print(f"PASS actual attach fail-closed: {failure}")


def attach_reject(listener, client, peers, failure):
    peer, request = accept_peer(listener, client)
    peers.append(peer)
    if failure == "old-server":
        peer.socket.setblocking(True)
        peer.socket.sendall(layout_message())
        peer.socket.setblocking(False)
        client.send(b"must not become legacy INPUT")
    elif failure == "state-key":
        peer.send(STATE, state(FAKE_EPOCH, OTHER))
    elif failure == "state-counter":
        peer.send(STATE, state(FAKE_EPOCH, request[20:36], 1))
    elif failure == "state-malformed":
        peer.send(STATE, b"\x02" + state(FAKE_EPOCH, request[20:36])[1:])
    elif failure == "state-busy":
        peer.send(STATE, state(FAKE_EPOCH, request[20:36], status=1))
    else:
        ready(peer, request)
        client.send(b"one")
        assert reliable_data(peer, 1) == b"one"
        peer.send(ACK, ack(2) if failure == "ack-future" else b"\x01\0\0\x01" + ack(1)[4:])
    deadline = time.monotonic() + (6 if failure == "old-server" else 3)
    while client.process.poll() is None and time.monotonic() < deadline:
        peer.pump()
    client.pump(0)
    assert client.process.poll() is not None and client.process.returncode != 0, (failure, client.logs())
    assert not any(kind == 3 for kind, _ in peer.seen), "failed reliable negotiation leaked legacy input"
    if failure == "old-server":
        assert not any(kind == DATA for kind, _ in peer.seen), "input sent without READY"
    assert client.terminal.flags == 0 and termios.tcgetattr(client.slave) == client.original_termios
    assert client.logs().strip(), "protocol failure was silent"
    print(f"PASS actual attach fail-closed: {failure}")


def actual_pair(server_binary, attach_binary, library, folder):
    server = OwnedServer(server_binary, folder / "actual-pair")
    client = None
    try:
        text = "ordered input\n" * 5000
        client = Attach(attach_binary, server.endpoint, library, folder,
                        ["--token", TOKEN.decode(), "--reliable-input", "--send", text])
        client.wait(lambda: server.received() == text.encode(),
                    "real server/attach input differed", timeout=10)
        # Drain queued ACKs before explicit detach, then prove CLOSE released
        # ownership by acquiring a different identity on the same server.
        for _ in range(10):
            client.pump(.02)
        client.detach()
        peer = server.peer()
        peer.send(OPEN, opening(key=OTHER))
        _, accepted = decode_state(peer.wait(STATE), key=OTHER)
        assert accepted == 0
        peer.send(CLOSE, ack(0))
        peer.closed()
        server.expect(text.encode())
        print(f"PASS actual server + attach: {len(text)} exact bytes, graceful lease release and reacquisition")
    finally:
        if client:
            client.close()
        server.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=Path("build/kmx-serve"))
    parser.add_argument("--attach", type=Path, default=Path("build/kmx-attach"))
    parser.add_argument("--library", type=Path, default=Path("build/libkilix-mux.so"))
    parser.add_argument("--expiry", action="store_true")
    args = parser.parse_args()
    library = load_terminal_library(args.library)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="kmx-input-resume-") as directory:
        folder = Path(directory)
        server_protocol(args.server.resolve(), folder, args.expiry)
        actual_pair(args.server.resolve(), args.attach.resolve(), library, folder)
        for partial in (False, True):
            client_fixture(args.attach.resolve(), library, folder, "partial" if partial else "lost-ack",
                lambda listener, client, peers: attach_resume(listener, client, peers, partial))
        client_fixture(args.attach.resolve(), library, folder, "handshake-retry", attach_handshake_retry)
        text = "x" * 70000
        client_fixture(args.attach.resolve(), library, folder, "send-chunks",
            lambda listener, client, peers: attach_send_chunks(listener, client, peers, text), options=["--send", text])
        for failure in ("resume-epoch", "resume-counter"):
            client_fixture(args.attach.resolve(), library, folder, failure,
                lambda listener, client, peers: attach_resume_refused(listener, client, peers, failure))
        for failure in ("state-key", "state-counter", "state-malformed", "state-busy", "ack-future", "ack-malformed", "old-server"):
            client_fixture(args.attach.resolve(), library, folder, failure,
                lambda listener, client, peers: attach_reject(listener, client, peers, failure), reconnect=0)
    print(f"input resume integration passed in {time.monotonic() - started:.2f}s")


if __name__ == "__main__":
    main()
