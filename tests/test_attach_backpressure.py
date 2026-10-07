#!/usr/bin/env python3
"""Owned actual attach PTYs with stalled plain Unix and pinned TLS peers."""
import argparse
import hashlib
import os
from pathlib import Path
import select
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

from test_attach_modes import Attach, ROOT, RESET, greet, layout_message, mode_message, load_terminal_library
from coding_bench import frame, next_frame

PAYLOAD = (b"coding paste: alpha beta gamma 0123456789\n" * 220000)[:8 * 1024 * 1024]


class Listener:
    def __init__(self, folder, tls=False):
        self.tls = tls
        self.context = None
        self.options = []
        self.socket = socket.socket(socket.AF_INET if tls else socket.AF_UNIX)
        if tls:
            certificate, key = folder / "cert.pem", folder / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-subj", "/CN=kmx-owned-test", "-days", "1", "-keyout", str(key),
                "-out", str(certificate)], check=True, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=10)
            fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate.read_text())).hexdigest()
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.load_cert_chain(certificate, key)
            self.socket.bind(("127.0.0.1", 0))
            self.endpoint = f"127.0.0.1:{self.socket.getsockname()[1]}"
            self.options = ["--tls-fingerprint", fingerprint]
        else:
            self.endpoint = folder / "peer.sock"
            self.socket.bind(str(self.endpoint))
        self.socket.listen(4)
        self.socket.settimeout(4)

    def accept(self):
        connection, address = self.socket.accept()
        if self.context:
            connection.settimeout(4)
            connection = self.context.wrap_socket(connection, server_side=True)
        return connection, address

    def close(self):
        self.socket.close()


class Paste:
    def __init__(self, client):
        self.client = client
        self.accepted = 0
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.submit)
        self.thread.start()

    def submit(self):
        try:
            while self.accepted < len(PAYLOAD) and not self.stop.is_set():
                if self.client.process.poll() is not None:
                    break
                if not select.select([], [self.client.master], [], .02)[1]:
                    continue
                try:
                    self.accepted += os.write(self.client.master,
                        PAYLOAD[self.accepted:self.accepted + 16384])
                except BlockingIOError:
                    continue
        except BaseException as error:
            self.error = error

    def close(self):
        self.stop.set()
        self.thread.join(2)
        assert not self.thread.is_alive(), "owned paste submitter did not stop"
        assert not self.error, self.error


def wait_stalled(client, paste):
    deadline = time.monotonic() + 5
    previous = -1
    stable_at = time.monotonic()
    while time.monotonic() < deadline:
        client.pump(.02)
        assert client.process.poll() is None, f"attach exited under output pressure: {client.logs()}"
        if paste.accepted != previous:
            previous, stable_at = paste.accepted, time.monotonic()
        if 128 * 1024 < previous < len(PAYLOAD) and time.monotonic() - stable_at >= .15:
            return
    raise AssertionError(f"paste never reached bounded backpressure: accepted={paste.accepted}")


def cell_update(sequence):
    # Existing raw codec form: blank viewport, no replaced rows, visible cursor.
    raw = b"KMX1" + bytes([12, 60, 0, 0, 1, 0])
    return frame(2, b"\0" + bytes([sequence, 0, len(raw)]) + raw)


def drain(peer, client, wanted, timeout=12):
    pending = bytearray()
    received = bytearray()
    acknowledgements, resizes, kinds = [], 0, []
    deadline = time.monotonic() + timeout
    peer.settimeout(.02)
    while time.monotonic() < deadline:
        client.pump(0)
        assert client.process.poll() is None, client.logs()
        try:
            data = peer.recv(65536)
        except (socket.timeout, ssl.SSLWantReadError):
            continue
        assert data, "attach closed its transport while draining"
        pending.extend(data)
        while (message := next_frame(pending)) is not None:
            kind, payload, consumed = message
            del pending[:consumed]
            kinds.append(kind)
            if kind == 3:
                received.extend(payload)
            elif kind == 5:
                assert len(payload) == 9 and payload[0] == 0, "invalid ACK frame"
                acknowledgements.append(int.from_bytes(payload[1:], "big"))
            elif kind == 4:
                assert len(payload) == 4, "invalid resize frame"
                resizes += 1
        if len(received) >= wanted and 1 in acknowledgements and resizes:
            return bytes(received), acknowledgements, resizes, kinds
    raise AssertionError(f"drain timed out: input={len(received)}/{wanted}, ACKs={acknowledgements}, resizes={resizes}")


def integrity(attach, lib, tls):
    name = "TLS" if tls else "plain"
    with tempfile.TemporaryDirectory(prefix="kmx-attach-pressure-") as directory:
        folder = Path(directory)
        listener = Listener(folder, tls)
        client = Attach(attach, listener.endpoint, lib, folder,
                        ["--reconnect", "0", *listener.options])
        peer = paste = None
        try:
            peer = greet(listener, client)
            peer.sendall(layout_message() + mode_message())
            client.wait_flags(79)
            paste = Paste(client)
            wait_stalled(client, paste)
            stalled_bytes = paste.accepted
            peer.sendall(mode_message(0) + cell_update(1))
            started = time.monotonic()
            client.wait_flags(0)
            assert time.monotonic() - started < 1, "incoming modes blocked behind network writes"
            os.kill(client.process.pid, signal.SIGWINCH)
            # The peer still refuses reads while ACK and resize join a pending
            # write. Under TLS this exercises retry length after queue growth.
            for _ in range(10):
                client.pump(.01)
            data, acks, resizes, _ = drain(peer, client, len(PAYLOAD))
            paste.close()
            assert paste.accepted == len(PAYLOAD)
            assert data == PAYLOAD, f"paste mismatch: got={len(data)}, want={len(PAYLOAD)}"
            assert 1 in acks and resizes >= 1
            client.detach()
            print(f"PASS {name} stopped reader: {len(data)} paste bytes intact; blocked at {stalled_bytes}; ACK/resize and incoming modes responsive")
        finally:
            if paste:
                paste.close()
            if peer:
                peer.close()
            client.close()
            listener.close()


def timed_exit(attach, lib, tls):
    name = "TLS" if tls else "plain"
    with tempfile.TemporaryDirectory(prefix="kmx-attach-seconds-") as directory:
        folder = Path(directory)
        listener = Listener(folder, tls)
        client = Attach(attach, listener.endpoint, lib, folder,
                        ["--reconnect", "0", "--seconds", "2", *listener.options])
        peer = paste = None
        started = time.monotonic()
        try:
            peer = greet(listener, client)
            peer.sendall(layout_message() + mode_message())
            client.wait_flags(79)
            paste = Paste(client)
            wait_stalled(client, paste)
            client.wait(lambda: client.process.poll() is not None,
                        "--seconds did not stop under network backpressure", timeout=3)
            elapsed = time.monotonic() - started
            assert elapsed < 3, elapsed
            assert client.terminal.flags == 0 and RESET in client.output
            assert "discarded" in client.logs() and "pending transport bytes" in client.logs(), client.logs()
            print(f"PASS {name} --seconds and terminal cleanup stay responsive ({elapsed:.2f}s)")
        finally:
            if paste:
                paste.close()
            if peer:
                peer.close()
            client.close()
            listener.close()


def reconnect_discard(attach, lib, tls):
    name = "TLS" if tls else "plain"
    with tempfile.TemporaryDirectory(prefix="kmx-attach-discard-") as directory:
        folder = Path(directory)
        listener = Listener(folder, tls)
        client = Attach(attach, listener.endpoint, lib, folder,
                        ["--reconnect", "3", *listener.options])
        peer = paste = None
        try:
            peer = greet(listener, client)
            peer.sendall(layout_message() + mode_message())
            client.wait_flags(79)
            paste = Paste(client)
            wait_stalled(client, paste)
            paste.close()
            reset_count = client.output.count(RESET)
            # Abrupt transport loss avoids TLS shutdown trying to drain input.
            peer.shutdown(socket.SHUT_RDWR)
            peer.close()
            peer = None
            client.wait(lambda: client.output.count(RESET) > reset_count,
                        "lost transport did not release input modes")
            assert "discarded" in client.logs() and "input delivery is unconfirmed" in client.logs(), client.logs()
            peer = greet(listener, client)
            peer.sendall(layout_message() + mode_message(0))
            client.wait_flags(0)
            peer.settimeout(.02)
            pending = bytearray()
            deadline = time.monotonic() + .25
            seen = []
            while time.monotonic() < deadline:
                client.pump(.01)
                try:
                    pending.extend(peer.recv(65536))
                except socket.timeout:
                    continue
                while (message := next_frame(pending)) is not None:
                    kind, _, consumed = message
                    del pending[:consumed]
                    seen.append(kind)
            assert 3 not in seen, f"uncertain old input replayed on new transport: {seen}"
            client.detach()
            print(f"PASS {name} reconnect discards/reports uncertain pending data and starts fresh")
        finally:
            if paste:
                paste.close()
            if peer:
                peer.close()
            client.close()
            listener.close()


def control_limit(attach, lib):
    with tempfile.TemporaryDirectory(prefix="kmx-attach-control-bound-") as directory:
        folder = Path(directory)
        listener = Listener(folder)
        client = Attach(attach, listener.endpoint, lib, folder, ["--reconnect", "0"])
        peer = greet(listener, client)
        sender = None
        try:
            peer.sendall(layout_message() + mode_message())
            client.wait_flags(79)
            peer.settimeout(5)
            def flood():
                try:
                    # A peer that keeps demanding ACKs while refusing to read
                    # them eventually consumes even the bounded control room.
                    peer.sendall(cell_update(1) * 1000000)
                except OSError:
                    pass
            sender = threading.Thread(target=flood)
            sender.start()
            client.wait(lambda: client.process.poll() is not None,
                        "outgoing control hard bound did not fail", timeout=12)
            assert client.process.returncode != 0
            assert "outgoing frame queue exhausted or failed" in client.logs(), client.logs()
            assert client.terminal.flags == 0 and RESET in client.output
            print("PASS control queue hard bound fails explicitly and restores terminal")
        finally:
            peer.close()
            if sender:
                sender.join(6)
                assert not sender.is_alive(), "owned control sender did not stop"
            client.close()
            listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--attach", type=Path, default=ROOT / "build/kmx-attach")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    parser.add_argument("--plain-only", action="store_true")
    args = parser.parse_args()
    lib = load_terminal_library(args.library)
    for tls in ([False] if args.plain_only else [False, True]):
        integrity(args.attach.resolve(), lib, tls)
        timed_exit(args.attach.resolve(), lib, tls)
        reconnect_discard(args.attach.resolve(), lib, tls)
    control_limit(args.attach.resolve(), lib)


if __name__ == "__main__":
    main()
