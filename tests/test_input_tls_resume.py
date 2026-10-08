#!/usr/bin/env python3
"""Real kmx-attach and kmx-serve TLS recovery through an owned fault proxy.

Both TLS legs complete real handshakes and pin their respective certificates.
The proxy decrypts only this fixture's traffic to cut at exact KMX boundaries.
No production executable has a fault hook.
"""
import argparse
import hashlib
from pathlib import Path
import select
import socket
import ssl
import struct
import tempfile
import threading

from test_attach_backpressure import Listener
from test_attach_modes import Attach, load_terminal_library
from test_input_resume import OwnedServer, TOKEN, DATA, ACK, STATE, OPEN, CLOSE, next_frame


class FaultProxy:
    def __init__(self, folder, endpoint, fingerprint, fault):
        self.listener = Listener(folder, tls=True)
        self.endpoint = self.listener.endpoint
        self.options = self.listener.options
        host, port = endpoint.rsplit(':', 1)
        self.backend = (host, int(port))
        self.fingerprint = fingerprint
        self.fault = fault
        self.faulted = threading.Event()
        self.stop = threading.Event()
        self.error = None
        self.connections = 0
        self.states = []
        self.opens = []
        self.data = []
        self.closes = []
        self.active = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    front, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.active = [front]
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
                # Verification is an exact pin, never an unverified test peer.
                back = context.wrap_socket(socket.create_connection(self.backend, timeout=3),
                                           server_hostname='localhost')
                self.active.append(back)
                assert hashlib.sha256(back.getpeercert(binary_form=True)).hexdigest() == self.fingerprint
                front.settimeout(2)
                back.settimeout(2)
                self.connections += 1
                try:
                    self.forward(front, back)
                finally:
                    for peer in self.active:
                        peer.close()
                    self.active = []
        except BaseException as error:
            if not self.stop.is_set():
                self.error = error

    def forward(self, front, back):
        pending = {front: bytearray(), back: bytearray()}
        while not self.stop.is_set():
            ready = [peer for peer in (front, back) if peer.pending()]
            if not ready:
                ready, _, _ = select.select([front, back], [], [], .02)
            for peer in ready:
                chunk = peer.recv(65536)
                if not chunk:
                    return
                pending[peer].extend(chunk)
                while (parsed := next_frame(pending[peer])) is not None:
                    kind, payload, length = parsed
                    wire = bytes(pending[peer][:length])
                    del pending[peer][:length]
                    destination = back if peer is front else front
                    if peer is front:
                        if kind == OPEN:
                            self.opens.append(bytes(payload))
                        if kind == CLOSE:
                            self.closes.append(bytes(payload))
                        if kind == DATA:
                            self.data.append((self.connections, struct.unpack('!Q', payload[4:12])[0], bytes(payload[12:])))
                            if self.fault == 'partial-data' and not self.faulted.is_set():
                                # Send a valid frame prefix plus five DATA-header
                                # bytes inside TLS, then lose both connections.
                                destination.sendall(wire[:len(wire) - len(payload) + 5])
                                self.faulted.set()
                                return
                    else:
                        if kind == STATE:
                            self.states.append(bytes(payload))
                        if kind == ACK and self.fault == 'lost-ack' and not self.faulted.is_set():
                            self.faulted.set()
                            return
                    destination.sendall(wire)

    def check(self):
        assert self.error is None, f'TLS proxy failed: {self.error!r}'

    def close(self):
        self.stop.set()
        self.listener.close()
        for peer in list(self.active):
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(5)
        assert not self.thread.is_alive(), 'TLS proxy did not stop'
        for peer in self.active:
            peer.close()
        self.check()


def recovery(server_binary, attach_binary, library, fault):
    with tempfile.TemporaryDirectory(prefix='kmx-input-tls-') as directory:
        folder = Path(directory)
        server = OwnedServer(server_binary, folder / 'server', tls=True)
        proxy = client = None
        try:
            proxy = FaultProxy(folder, server.endpoint, server.fingerprint, fault)
            client = Attach(attach_binary, proxy.endpoint, library, folder,
                            ['--token', TOKEN.decode(), '--reliable-input', '--reconnect', '3', *proxy.options])
            def wait(predicate, message):
                def checked():
                    proxy.check()
                    return predicate()
                client.wait(checked, message, timeout=10)
            wait(lambda: len(proxy.states) == 1, 'TLS input selection failed')
            first = b'first input over real TLS\n'
            client.send(first)
            wait(lambda: proxy.faulted.is_set() and len(proxy.states) == 2,
                 'TLS reconnect failed')
            wait(lambda: server.received() == first, 'TLS replay lost or duplicated first input')
            second = b'after reconnect\n'
            client.send(second)
            wait(lambda: server.received() == first + second, 'post-reconnect TLS input differed')
            for _ in range(10):
                client.pump(.02)
            client.detach()
            wait(lambda: len(proxy.closes) == 1, 'explicit TLS CLOSE missing')
            assert proxy.connections == 2, proxy.connections
            assert len(proxy.opens) == 2
            epoch = proxy.states[0][4:20]
            assert epoch != bytes(16) and proxy.states[1][4:20] == epoch
            assert proxy.opens[1][4:20] == epoch
            assert proxy.opens[1][20:36] == proxy.opens[0][20:36]
            assert proxy.opens[1][36:] == bytes(8), 'client acknowledged lost input'
            accepted = struct.unpack('!Q', proxy.states[1][36:44])[0]
            assert accepted == (1 if fault == 'lost-ack' else 0), accepted
            sequences = [(connection, seq) for connection, seq, _ in proxy.data]
            expected = [(1, 1), (2, 2)] if fault == 'lost-ack' else [(1, 1), (2, 1), (2, 2)]
            assert sequences == expected, (sequences, expected)
            assert server.received() == first + second
            print(f'PASS real server + attach TLS {fault}: two pinned handshakes, resume watermark={accepted}, exact input, sequences={sequences}, CLOSE and terminal cleanup')
        finally:
            try:
                if client:
                    client.close()
            finally:
                try:
                    if proxy:
                        proxy.close()
                finally:
                    server.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=Path('build/kmx-serve'))
    parser.add_argument('--attach', type=Path, default=Path('build/kmx-attach'))
    parser.add_argument('--library', type=Path, default=Path('build/libkilix-mux.so'))
    args = parser.parse_args()
    library = load_terminal_library(args.library)
    for fault in ('lost-ack', 'partial-data'):
        recovery(args.server.resolve(), args.attach.resolve(), library, fault)


if __name__ == '__main__':
    main()
