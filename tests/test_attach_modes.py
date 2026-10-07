#!/usr/bin/env python3
"""Actual attach PTY regression with libvterm-generated paste and arrow input.

python3 tests/test_attach_modes.py --server build/kmx-serve \
    --attach build/kmx-attach --library build/libkilix-mux.so
"""
import argparse
import ctypes as C
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from coding_bench import frame, next_frame, stop_owned_child, stop_process

ENABLED = 1 | 2 | 4 | 8 | 64
RESET = b"\x1b[?1l\x1b[?2004l\x1b[?1004l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?25h"
CHILD = r'''
import json,os,select,sys,time,tty
from pathlib import Path
result,pid,ready=sys.argv[1:]
Path(pid).write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
tty.setraw(0)
enabled=b"\x1b[?1;2004;1004;1000;1006h"
os.write(1,enabled[:-1]); time.sleep(.02); os.write(1,enabled[-1:])
Path(ready).write_text("ready")
received=b""; deadline=time.monotonic()+20
while time.monotonic()<deadline:
    if not select.select([0],[],[],.1)[0]: continue
    data=os.read(0,65536)
    if not data: break
    received+=data
    temporary=Path(result+".tmp"); temporary.write_text(json.dumps({"hex":received.hex()})); temporary.replace(result)
    if b"TOGGLE" in data: os.write(1,b"\x1b[?1;2004;1004;1000;1006l")
    if b"ENABLE" in data: os.write(1,enabled)
'''


class Terminal:
    """A native terminal interprets attach output and generates its own input."""
    def __init__(self, library):
        self.lib = library
        self.vt = library.vterm_new(12, 60)
        assert self.vt
        library.vterm_set_utf8(self.vt, 1)
        state = library.vterm_obtain_state(self.vt)
        library.vterm_state_reset(state, 1)
        self.generated = bytearray()
        callback_type = C.CFUNCTYPE(None, C.c_void_p, C.c_size_t, C.c_void_p)
        self.callback = callback_type(lambda pointer, size, user:
                                      self.generated.extend(C.string_at(pointer, size)))
        library.vterm_output_set_callback(self.vt, self.callback, None)
        self.observer = C.c_void_p()
        assert library.kmx_modes_create(C.byref(self.observer)) == 0

    def feed(self, data):
        assert self.lib.vterm_input_write(self.vt, data, len(data)) == len(data)
        assert self.lib.kmx_modes_feed(self.observer, data, len(data)) == 0

    @property
    def flags(self):
        return self.lib.kmx_modes_get(self.observer)

    def paste_and_up(self, text):
        self.generated.clear()
        self.lib.vterm_keyboard_start_paste(self.vt)
        self.generated.extend(text)
        self.lib.vterm_keyboard_end_paste(self.vt)
        self.lib.vterm_keyboard_key(self.vt, 5, 0)  # VTERM_KEY_UP, no modifiers
        return bytes(self.generated)

    def close(self):
        self.lib.kmx_modes_free(self.observer)
        self.lib.vterm_free(self.vt)


def load_terminal_library(path):
    lib = C.CDLL(str(path.resolve()))
    signatures = {
        "vterm_new": ([C.c_int, C.c_int], C.c_void_p),
        "vterm_free": ([C.c_void_p], None),
        "vterm_set_utf8": ([C.c_void_p, C.c_int], None),
        "vterm_obtain_state": ([C.c_void_p], C.c_void_p),
        "vterm_state_reset": ([C.c_void_p, C.c_int], None),
        "vterm_input_write": ([C.c_void_p, C.c_void_p, C.c_size_t], C.c_size_t),
        "vterm_output_set_callback": ([C.c_void_p, C.c_void_p, C.c_void_p], None),
        "vterm_keyboard_start_paste": ([C.c_void_p], None),
        "vterm_keyboard_end_paste": ([C.c_void_p], None),
        "vterm_keyboard_key": ([C.c_void_p, C.c_int, C.c_int], None),
        "kmx_modes_create": ([C.POINTER(C.c_void_p)], C.c_int),
        "kmx_modes_free": ([C.c_void_p], None),
        "kmx_modes_feed": ([C.c_void_p, C.c_void_p, C.c_size_t], C.c_int),
        "kmx_modes_get": ([C.c_void_p], C.c_uint32),
    }
    for name, (args, result) in signatures.items():
        function = getattr(lib, name)
        function.argtypes, function.restype = args, result
    return lib


class Attach:
    def __init__(self, binary, endpoint, lib, folder, options=()):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 60, 0, 0))
        self.original_termios = termios.tcgetattr(self.slave)
        self.terminal = Terminal(lib)
        self.output = bytearray()
        self.log = tempfile.TemporaryFile(dir=folder)
        self.process = subprocess.Popen([str(binary), "--socket", str(endpoint),
            "--no-predict", "--audio-codec", "pcm", *options],
            stdin=self.slave, stdout=self.slave, stderr=self.log)
        os.set_blocking(self.master, False)

    def pump(self, timeout=.02):
        if not select.select([self.master], [], [], timeout)[0]:
            return
        try:
            data = os.read(self.master, 65536)
        except OSError as error:
            if error.errno in (errno.EAGAIN, errno.EIO):
                return
            raise
        if data:
            self.output.extend(data)
            self.terminal.feed(data)

    def wait(self, predicate, message, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            if predicate():
                return
            if self.process.poll() is not None:
                self.pump(0)
                break
        assert predicate(), f"{message}: rc={self.process.poll()}, flags={self.terminal.flags}, log={self.logs()}"

    def wait_flags(self, flags):
        self.wait(lambda: self.terminal.flags == flags, f"expected terminal modes {flags}")

    def send(self, data):
        assert os.write(self.master, data) == len(data)

    def logs(self):
        self.log.seek(0)
        return self.log.read().decode(errors="replace")

    def detach(self):
        self.send(b"\x1d")
        self.wait(lambda: self.process.poll() is not None, "attach did not detach")
        self.pump(0)
        assert self.process.returncode == 0, self.logs()
        assert self.terminal.flags == 0, "detach retained terminal modes"
        assert RESET in self.output, "detach did not reset input modes and show cursor"
        assert termios.tcgetattr(self.slave) == self.original_termios, "detach did not restore termios"

    def close(self):
        stop_process(self.process)
        self.pump(0)
        self.terminal.close()
        os.close(self.master)
        os.close(self.slave)
        self.log.close()


def wait_path(path, process, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not path.exists():
        assert process.poll() is None, "server exited before readiness"
        time.sleep(.01)
    assert path.exists(), f"missing readiness file {path}"


def server_roundtrip(server, attach, lib):
    with tempfile.TemporaryDirectory(prefix="kmx-attach-modes-") as directory:
        folder = Path(directory)
        endpoint = folder / "server.sock"
        result, pid, ready = [folder / name for name in ("result.json", "pid.json", "ready")]
        with tempfile.TemporaryFile(dir=folder) as log:
            process = subprocess.Popen([str(server), "--socket", str(endpoint),
                "--rows", "12", "--cols", "60", "--", sys.executable,
                "-c", CHILD, str(result), str(pid), str(ready)], stdout=log, stderr=log)
            client = None
            try:
                wait_path(ready, process)
                client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0"])
                client.wait_flags(ENABLED)
                payload = client.terminal.paste_and_up(b"first line\nsecond line\n")
                assert payload == b"\x1b[200~first line\nsecond line\n\x1b[201~\x1bOA", payload
                client.send(payload)
                client.wait(lambda: result.exists() and bytes.fromhex(json.loads(result.read_text())["hex"]) == payload,
                            "PTY owner did not receive bracketed multiline paste and application arrow")
                client.send(b"TOGGLE")
                client.wait_flags(0)
                normal = client.terminal.paste_and_up(b"normal\n")
                assert normal == b"normal\n\x1b[A", normal
                client.send(normal)
                wanted = payload + b"TOGGLE" + normal
                client.wait(lambda: bytes.fromhex(json.loads(result.read_text())["hex"]) == wanted,
                            "PTY owner did not receive normal-mode input after mode change")
                client.send(b"ENABLE")
                client.wait_flags(ENABLED)
                client.detach()
                client.close()
                client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0"])
                client.wait_flags(ENABLED)
                client.detach()
                print("PASS real server/attach PTYs: fragmented modes, multiline paste, arrows, changes, late attach, detach")
            finally:
                if client:
                    client.close()
                stop_owned_child(pid)
                stop_process(process)


def layout_message(count=1, focused=0):
    payload = bytearray([12, 60, 1, count])
    width = 60 if count == 1 else 29
    for slot in range(count):
        payload.extend([slot + 1, slot * 30, 1 if count > 1 else 0,
                        11 if count > 1 else 12, width, int(slot == focused), 0])
    return frame(7, payload)


def server_focus(server, attach, lib):
    with tempfile.TemporaryDirectory(prefix="kmx-mode-focus-") as directory:
        folder = Path(directory)
        endpoint = folder / "focus.sock"
        commands, paths = [], []
        for slot in range(2):
            result, pid, ready = [folder / f"{name}-{slot}" for name in ("result", "pid", "ready")]
            source = CHILD if slot == 0 else CHILD.replace(
                'enabled=b"\\x1b[?1;2004;1004;1000;1006h"', 'enabled=b""')
            commands.extend(["--pane", shlex.join([sys.executable, "-c", source,
                                                   str(result), str(pid), str(ready)])])
            paths.append((result, pid, ready))
        with tempfile.TemporaryFile(dir=folder) as log:
            process = subprocess.Popen([str(server), "--socket", str(endpoint),
                                        "--rows", "12", "--cols", "60", *commands],
                                       stdout=log, stderr=log)
            client = None
            try:
                for _, _, ready in paths:
                    wait_path(ready, process)
                client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0"])
                client.wait_flags(ENABLED & ~8)
                client.send(b"\x0f")  # Ctrl-O changes authoritative focus.
                client.wait_flags(0)
                client.send(b"second pane input")
                client.wait(lambda: paths[1][0].exists() and
                            bytes.fromhex(json.loads(paths[1][0].read_text())["hex"]) == b"second pane input",
                            "input did not follow server focus")
                assert not paths[0][0].exists(), "unfocused pane received focused input"
                client.send(b"\x0f")
                client.wait_flags(ENABLED & ~8)
                client.detach()
                print("PASS actual two-pane focus changes modes and input target; mouse capture remains off")
            finally:
                if client:
                    client.close()
                for _, pid, _ in paths:
                    stop_owned_child(pid)
                stop_process(process)


def server_reply_limit(server):
    """A query producer that never reads its replies must fail explicitly."""
    child = r'''
import json,os,sys,tty
from pathlib import Path
tty.setraw(0)
Path(sys.argv[1]).write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
for _ in range(100): os.write(1,b"\x1b[6n"*100000)
'''
    with tempfile.TemporaryDirectory(prefix="kmx-reply-bound-") as directory:
        folder = Path(directory)
        pid = folder / "pid"
        with tempfile.TemporaryFile(dir=folder) as log:
            process = subprocess.Popen([str(server), "--socket", str(folder / "bound.sock"),
                "--rows", "12", "--cols", "60", "--", sys.executable,
                "-c", child, str(pid)], stdout=log, stderr=log)
            try:
                code = process.wait(timeout=10)
                log.seek(0)
                message = log.read().decode(errors="replace")
                assert code != 0, "exhausted reply queue silently returned success"
                assert "terminal reply queue exhausted or failed" in message, message
                print("PASS bounded terminal reply flood fails explicitly")
            finally:
                stop_owned_child(pid)
                stop_process(process)


def mode_message(flags=ENABLED, pane=0):
    return frame(14, struct.pack("!BBHI", 1, pane, 0, flags))


def greet(listener, client):
    connection, _ = listener.accept()
    connection.settimeout(.02)
    pending = bytearray()
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        client.pump(0)
        try:
            pending.extend(connection.recv(65536))
        except socket.timeout:
            continue
        while (message := next_frame(pending)) is not None:
            kind, _, consumed = message
            del pending[:consumed]
            if kind == 1:
                return connection
    connection.close()
    raise AssertionError("attach did not greet fake server")


def fake_server_cases(attach, lib):
    with tempfile.TemporaryDirectory(prefix="kmx-mode-peer-") as directory:
        folder = Path(directory)
        endpoint = folder / "peer.sock"
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(str(endpoint))
            listener.listen(4)
            listener.settimeout(4)
            client = Attach(attach, endpoint, lib, folder, ["--reconnect", "3"])
            connection = None
            try:
                connection = greet(listener, client)
                connection.sendall(layout_message() + mode_message())
                client.wait_flags(ENABLED)
                reset_count = client.output.count(RESET)
                connection.close()
                connection = None
                client.wait(lambda: client.output.count(RESET) > reset_count,
                            "disconnect did not reset modes before reconnect")
                client.wait_flags(0)
                connection = greet(listener, client)
                connection.sendall(layout_message() + mode_message())
                client.wait_flags(ENABLED)
                client.detach()
                print("PASS reconnect resets modes and applies fresh focused state")
            finally:
                if connection:
                    connection.close()
                client.close()

            # An old server omits type 14. Input remains ordinary and unknown
            # framed extensions remain ignorable.
            client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0"])
            connection = greet(listener, client)
            try:
                connection.sendall(layout_message() + frame(250, b"optional future extension"))
                client.wait(lambda: b"\x1b[0m" in client.output, "old-server layout did not render")
                assert client.terminal.flags == 0
                client.detach()
                print("PASS old server and unknown framed extension compatibility")
            finally:
                connection.close()
                client.close()

            for options, expected in [(["--view"], 0), (["--dump"], 0),
                                      (["--pixel-input"], 2 | 32 | 64), ([], 1 | 2 | 4 | 64)]:
                client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0", *options])
                connection = greet(listener, client)
                try:
                    count = 2 if not options else 1
                    connection.sendall(layout_message(count) + mode_message() + frame(250, b"ignored"))
                    # Wait for the known mode frame to be processed before assertions.
                    for _ in range(20):
                        client.pump(.01)
                    assert client.terminal.flags == expected, (options, client.terminal.flags, client.logs())
                    assert client.process.poll() is None, client.logs()
                    if "--dump" in options:
                        connection.sendall(frame(6, b""))
                        client.wait(lambda: client.process.poll() is not None, "dump did not exit")
                    else:
                        client.detach()
                finally:
                    connection.close()
                    client.close()
            print("PASS viewer/dump/pixel ownership and multi-pane mouse restriction")

            malformed = [
                b"", struct.pack("!BBHI", 2, 0, 0, 0),
                struct.pack("!BBHI", 1, 0, 1, 0), struct.pack("!BBHI", 1, 0, 0, 128),
                struct.pack("!BBHI", 1, 0, 0, 8 | 16),
                struct.pack("!BBHI", 1, 32, 0, 0), struct.pack("!BBHI", 1, 1, 0, 0),
            ]
            for index, payload in enumerate(malformed):
                client = Attach(attach, endpoint, lib, folder, ["--reconnect", "0"])
                connection = greet(listener, client)
                try:
                    count = 2 if index == len(malformed) - 1 else 1
                    connection.sendall(layout_message(count) + frame(14, payload))
                    client.wait(lambda: client.process.poll() is not None, "malformed mode frame was accepted")
                    assert client.process.returncode != 0, "malformed mode frame returned success"
                    assert "invalid terminal mode state" in client.logs(), client.logs()
                    assert client.terminal.flags == 0
                finally:
                    connection.close()
                    client.close()
            print("PASS malformed mode frames fail explicitly and restore input modes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, default=ROOT / "build/kmx-serve")
    parser.add_argument("--attach", type=Path, default=ROOT / "build/kmx-attach")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    args = parser.parse_args()
    lib = load_terminal_library(args.library)
    server_roundtrip(args.server.resolve(), args.attach.resolve(), lib)
    server_focus(args.server.resolve(), args.attach.resolve(), lib)
    server_reply_limit(args.server.resolve())
    fake_server_cases(args.attach.resolve(), lib)


if __name__ == "__main__":
    main()
