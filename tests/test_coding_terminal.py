#!/usr/bin/env python3
"""Bounded PTY-owner query regression; no KMX client attaches.

python3 tests/test_coding_terminal.py [--server /path/to/kmx-serve]
--expect-missing is an explicit baseline probe, not the regression's default.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from coding_bench import Receiver, frame, load_library, next_frame, stop_owned_child
CHILD = r'''
import json,os,select,sys,termios,time,tty
from pathlib import Path
result,pid=sys.argv[1:]
Path(pid).write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
original=termios.tcgetattr(0); tty.setraw(0)
received=b""
try:
    started=time.monotonic()
    os.write(1,b"\x1b[4;9H\x1b[6")
    time.sleep(.02)
    os.write(1,b"n\x1b[5n")
    deadline=time.monotonic()+2
    while time.monotonic()<deadline:
        if b"\x1b[4;9R" in received and b"\x1b[0n" in received: break
        ready,_,_=select.select([0],[],[],max(0,deadline-time.monotonic()))
        if not ready: break
        data=os.read(0,4096)
        if not data: break
        received+=data
    Path(result).write_text(json.dumps({"cursor_reply":b"\x1b[4;9R" in received,
        "status_reply":b"\x1b[0n" in received,"received_hex":received.hex(),"reply_ms":(time.monotonic()-started)*1000}))
finally: termios.tcsetattr(0,termios.TCSANOW,original)
'''


def run(server, expect_missing=False):
    with tempfile.TemporaryDirectory(prefix="kmx-query-") as directory:
        folder = Path(directory)
        result, pid = folder / "result.json", folder / "child.pid"
        with open(folder / "server.log", "wb") as log:
            process = subprocess.Popen([str(server), "--socket", str(folder / "query.sock"),
                "--rows", "12", "--cols", "60", "--", sys.executable, "-c", CHILD,
                str(result), str(pid)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 6
                while not result.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        break
                    time.sleep(.01)
                if not result.exists():
                    raise AssertionError("query child did not report within bounded timeout: " +
                                         (folder / "server.log").read_text())
                observed = json.loads(result.read_text())
                success = observed["cursor_reply"] and observed["status_reply"]
                if expect_missing:
                    assert not success, f"baseline unexpectedly answers both queries: {observed}"
                    print("BASELINE MISSING terminal query replies: " + json.dumps(observed))
                else:
                    assert success, f"PTY owner failed to answer terminal queries without a client: {observed}"
                    assert observed["reply_ms"] < 1000, f"local PTY query replies were delayed: {observed}"
                    print(f"PASS PTY owner answers fragmented cursor and status queries without a client in {observed['reply_ms']:.2f} ms")
                return observed
            finally:
                stop_owned_child(pid)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


BACKPRESSURE_CHILD = r'''
import hashlib,json,os,select,sys,termios,time,tty
from pathlib import Path
result,pid,release,ready,count,query=sys.argv[1:]; count=int(count); query=query=="1"
Path(pid).write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
original=termios.tcgetattr(0); tty.setraw(0)
try:
    Path(ready).write_text("ready")
    deadline=time.monotonic()+8
    tick=0
    while not Path(release).exists() and time.monotonic()<deadline:
        os.write(1,("\x1b[HPAUSED_READY %08d"%tick).encode()); tick+=1; time.sleep(.02)
    reply=b"\x1b[1;1R"
    if query:
        os.write(1,b"\x1b[H\x1b[6n")
        time.sleep(.1)  # Owner must handle this while the paste queue is still full.
    expected=count+(len(reply) if query else 0)
    received=bytearray(); deadline=time.monotonic()+8
    while len(received)<expected and time.monotonic()<deadline:
        if select.select([0],[],[],max(0,deadline-time.monotonic()))[0]:
            data=os.read(0,min(65536,expected-len(received)))
            if not data: break
            received.extend(data)
    clean=received.replace(reply,b"") if query else received
    observed={"bytes":len(clean),"sha256":hashlib.sha256(clean).hexdigest()}
    if query: observed["query_reply_count"]=received.count(reply)
    Path(result).write_text(json.dumps(observed))
finally: termios.tcsetattr(0,termios.TCSANOW,original)
'''


def wait_file(path, process, timeout=6):
    deadline = time.monotonic() + timeout
    while not path.exists() and time.monotonic() < deadline:
        if process.poll() is not None:
            break
        time.sleep(.01)
    assert path.exists(), f"owned child did not create {path.name} before timeout"


def backpressure(server, library, query=False):
    """A busy controller preserves all input without delaying another viewer."""
    payload = bytes(range(256)) * 4096
    with tempfile.TemporaryDirectory(prefix="kmx-pressure-") as directory:
        folder = Path(directory)
        result, pid, release, ready = [folder / name for name in ("result.json", "child.pid", "release", "ready")]
        endpoint = str(folder / "session.sock")
        controller = viewer = receiver = sender = None
        failures = []
        with open(folder / "server.log", "wb") as log:
            process = subprocess.Popen([str(server), "--socket", endpoint, "--rows", "12", "--cols", "60",
                "--", sys.executable, "-c", BACKPRESSURE_CHILD, str(result), str(pid), str(release),
                str(ready), str(len(payload)), "1" if query else "0"], stdout=log, stderr=log)
            try:
                wait_file(ready, process)
                controller = socket.socket(socket.AF_UNIX)
                controller.settimeout(8)
                controller.connect(endpoint)
                controller.sendall(frame(1, struct.pack(">HHB", 12, 60, 0)))
                def submit():
                    try:
                        for offset in range(0, len(payload), 4096):
                            controller.sendall(frame(3, payload[offset:offset + 4096]))
                    except BaseException as error:
                        failures.append(str(error))
                sender = threading.Thread(target=submit, daemon=True)
                sender.start()
                time.sleep(.2)
                viewer = socket.socket(socket.AF_UNIX)
                viewer.settimeout(.2)
                viewer.connect(endpoint)
                viewer.sendall(frame(1, struct.pack(">HHB", 12, 60, 1)))
                receiver = Receiver(load_library(library), 12, 60)
                pending = bytearray()
                screen_ticks = set()
                started = time.monotonic()
                deadline = started + 3
                while len(screen_ticks) < 2 and time.monotonic() < deadline:
                    try:
                        data = viewer.recv(65536)
                    except socket.timeout:
                        continue
                    assert data, "viewer disconnected while controller input was backpressured"
                    pending.extend(data)
                    while True:
                        parsed = next_frame(pending)
                        if parsed is None:
                            break
                        kind, body, length = parsed
                        del pending[:length]
                        if kind == 2:
                            assert body[0] == 0
                            sequence = receiver.apply(body[1:])
                            viewer.sendall(frame(5, bytes([0]) + struct.pack(">Q", sequence)))
                            grid = receiver.lib.kmx_receiver_grid(receiver.handle).contents
                            text = "".join(chr(grid.cells[col].chars[0] or 32) for col in range(grid.cols))
                            if text.startswith("PAUSED_READY "):
                                screen_ticks.add(text.split()[1])
                assert len(screen_ticks) >= 2, "viewer did not receive changing decoded screen while input was paused"
                assert process.poll() is None, "server exited while input was paused"
                release.write_text("drain")
                wait_file(result, process, timeout=10)
                sender.join(timeout=2)
                assert not sender.is_alive(), "controller failed to finish after the PTY resumed"
                assert not failures, f"controller send failed: {failures}"
                observed = json.loads(result.read_text())
                expected = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
                if query:
                    expected["query_reply_count"] = 1
                assert observed == expected, f"input or terminal reply was lost/reordered: got {observed}, expected {expected}"
                print("PASS 1 MiB input survives PTY backpressure; separate viewer receives changing decoded screen"
                      + ("; cursor query reply survives full input queue" if query else ""))
            finally:
                for sock in (controller, viewer):
                    if sock:
                        sock.close()
                if receiver:
                    receiver.close()
                stop_owned_child(pid)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
                if sender:
                    sender.join(timeout=1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=ROOT / "build/kmx-serve")
    parser.add_argument("--expect-missing", action="store_true")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    parser.add_argument("--query-only", action="store_true")
    args = parser.parse_args()
    run(args.server.resolve(), args.expect_missing)
    if not args.query_only and not args.expect_missing:
        backpressure(args.server.resolve(), args.library.resolve())
        backpressure(args.server.resolve(), args.library.resolve(), query=True)
