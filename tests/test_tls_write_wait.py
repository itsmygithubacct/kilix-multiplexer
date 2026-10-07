#!/usr/bin/env python3
"""Actual attach TLS with test-only WANT_READ -> WANT_WRITE fault injection.

Build the isolated client with tests/tls_write_wait_shim.c and linker wrappers
for SSL_write and SSL_get_error; the production executable has no test hook.
"""
import argparse
import os
from pathlib import Path
import re
import signal
import tempfile
import termios
import time

from test_attach_backpressure import Listener, cell_update, drain
from test_attach_modes import Attach, ROOT, RESET, layout_message, mode_message, load_terminal_library


def test_wait_direction(binary, library):
    payload = b"held input " + b"x" * 2048 + b"\n"
    with tempfile.TemporaryDirectory(prefix="kmx-tls-write-wait-") as directory:
        folder = Path(directory)
        listener = Listener(folder, True)
        client = peer = None
        try:
            client = Attach(binary, listener.endpoint, load_terminal_library(library), folder,
                            ["--reconnect", "0", "--seconds", "2", *listener.options])
            peer, _ = listener.accept()
            client.wait(lambda: "direction=READ" in client.logs(), "test write did not enter WANT_READ")
            client.send(payload)
            os.kill(client.process.pid, signal.SIGWINCH)
            quiet_until = time.monotonic() + .35
            while time.monotonic() < quiet_until:
                client.pump(.02)
                assert client.process.poll() is None, client.logs()
                assert "excessive_read_retries" not in client.logs(), "writable socket spins while TLS write needs READ"
            # Readability changes the pending TLS write to WANT_WRITE. Its
            # retry then succeeds, retaining bytes/length despite queue growth.
            peer.sendall(layout_message() + mode_message() + cell_update(1))
            client.wait_flags(79)
            data, acknowledgements, resizes, _ = drain(peer, client, len(payload), timeout=1)
            assert data == payload, "input framing changed across TLS WANT retries"
            assert 1 in acknowledgements and resizes >= 1
            client.wait(lambda: client.process.poll() is not None, "--seconds did not remain bounded", timeout=3)
            summary = re.search(r"TLS_WAIT_TEST phase=(\d+) read_retries=(\d+) write_retries=(\d+)", client.logs())
            assert summary, client.logs()
            phase, reads, writes = map(int, summary.groups())
            assert phase == 2 and 1 <= reads < 10 and writes == 1, client.logs()
            assert "direction=WRITE" in client.logs() and "resumed" in client.logs()
            assert client.process.returncode == 0, client.logs()
            assert client.terminal.flags == 0 and RESET in client.output
            assert termios.tcgetattr(client.slave) == client.original_termios
            print(f"PASS TLS WANT_READ quiet wait ({reads} attempts), WANT_WRITE resume, exact input/ACK/resize, bounded --seconds and terminal cleanup")
        finally:
            if peer:
                peer.close()
            if client:
                client.close()
            listener.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--attach", type=Path, default=ROOT / "build/kmx-attach-tls-write-wait")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    args = parser.parse_args()
    test_wait_direction(args.attach.resolve(), args.library.resolve())
