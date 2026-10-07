#!/usr/bin/env python3
"""Owned PTY regression for bounded DEC synchronized-output repaint holds."""
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
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from coding_bench import Receiver, frame, load_library, next_frame, stop_owned_child, stop_process

CHILD = r'''
import json,os,select,sys,time,tty
from pathlib import Path
folder=Path(sys.argv[1]); role=int(sys.argv[2]); tty.setraw(0)
(folder/f"pid-{role}").write_text(json.dumps({"pid":os.getpid(),"starttime":
    int(Path("/proc/self/stat").read_text().rsplit(")",1)[1].split()[19])}))
os.write(1,b"\x1b[HBASELINE_PRIMARY" if not role else b"\x1b[HBASELINE_PEER")
(folder/f"ready-{role}").touch()
started=finished=advanced=False; repeated=0.; data=b""; deadline=time.monotonic()+10
while time.monotonic()<deadline:
    if not role and not started and (folder/"begin").exists():
        os.write(1,b"\x1b[?2026"); time.sleep(.01)
        os.write(1,b"h\x1b[H\x1b[2JPARTIAL_FIRST\x1b[5n")
        (folder/"active").write_text(str(time.monotonic())); started=True
    if not role and started and not finished:
        if (folder/"finish").exists():
            os.write(1,b"\x1b[2;1HCOMPLETE_LAST\x1b[?2026"); time.sleep(.01); os.write(1,b"l")
            (folder/"finished").touch(); finished=True
        elif (folder/"repeat").exists() and time.monotonic()-repeated>.03:
            os.write(1,b"\x1b[?2026h"); repeated=time.monotonic()
        elif (folder/"pairs").exists() and time.monotonic()-repeated>.03:
            os.write(1,b"\x1b[?2026l\x1b[?2026h"); repeated=time.monotonic()
        if not advanced and (folder/"advance").exists():
            os.write(1,b"\x1b[3;1HPOST_TIMEOUT"); advanced=True
    if select.select([0],[],[],.005)[0]:
        received=os.read(0,4096)
        if not received: break
        data+=received
        (folder/f"input-{role}").write_text(data.hex())
        if role: os.write(1,b"\x1b[2;1HPEER_RESPONSIVE")
'''


class Peer:
    def __init__(self, endpoint, library, viewer=False):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.connect(str(endpoint))
        self.socket.sendall(frame(1, struct.pack("!HHB", 12, 60, int(viewer))))
        self.socket.setblocking(False)
        self.pending = bytearray()
        self.receivers = [Receiver(library, 12, 60) for _ in range(2)]
        self.states = []
        self.latest = ["", ""]

    def send(self, kind, payload):
        self.socket.sendall(frame(kind, payload))

    def pump(self):
        if select.select([self.socket], [], [], 0)[0]:
            data = self.socket.recv(65536)
            assert data, "server closed before synchronized-output test completed"
            self.pending.extend(data)
        while (message := next_frame(self.pending)) is not None:
            kind, payload, consumed = message
            del self.pending[:consumed]
            if kind != 2:
                continue
            pane = payload[0]
            assert pane < 2
            sequence = self.receivers[pane].apply(payload[1:])
            grid = self.receivers[pane].lib.kmx_receiver_grid(self.receivers[pane].handle).contents
            text = "".join(chr(grid.cells[index].chars[0] or 32)
                           for index in range(grid.rows * grid.cols))
            self.latest[pane] = text
            self.states.append((time.monotonic(), pane, text))
            self.send(5, bytes([pane]) + sequence.to_bytes(8, "big"))

    def close(self):
        self.socket.close()
        for receiver in self.receivers:
            receiver.close()


def until(peers, predicate, message, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for peer in peers:
            peer.pump()
        if predicate():
            return
        time.sleep(.003)
    assert predicate(), message


def run(server, library, hung=False, pairs=False):
    with tempfile.TemporaryDirectory(prefix="kmx-synchronized-") as directory:
        folder = Path(directory)
        endpoint = folder / "session.sock"
        commands = []
        for role in range(2):
            commands.extend(["--pane", shlex.join([sys.executable, "-c", CHILD, str(folder), str(role)])])
        with tempfile.TemporaryFile(dir=folder) as log:
            process = subprocess.Popen([str(server), "--socket", str(endpoint),
                                        "--rows", "12", "--cols", "60", *commands],
                                       stdout=log, stderr=log)
            peers = []
            try:
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline and not all((folder / f"ready-{role}").exists() for role in range(2)):
                    assert process.poll() is None
                    time.sleep(.005)
                assert all((folder / f"ready-{role}").exists() for role in range(2))
                peers = [Peer(endpoint, library), Peer(endpoint, library, True)]
                until(peers, lambda: all("BASELINE_PRIMARY" in peer.latest[0] and
                    "BASELINE_PEER" in peer.latest[1] for peer in peers), "initial viewport missing")
                if hung:
                    (folder / ("pairs" if pairs else "repeat")).touch()
                (folder / "begin").touch()
                until(peers, lambda: (folder / "active").exists() and (folder / "active").read_text(),
                      "child did not start synchronized output")
                started = float((folder / "active").read_text())
                # Replies and input remain live while the visible paint is held.
                peers[0].send(3, b"INPUT_WHILE_HELD")
                until(peers, lambda: (folder / "input-0").exists() and
                    b"\x1b[0n" in bytes.fromhex((folder / "input-0").read_text()) and
                    b"INPUT_WHILE_HELD" in bytes.fromhex((folder / "input-0").read_text()),
                    "terminal reply or input blocked by synchronized paint", timeout=.12)
                peers[0].send(8, b"\1")
                peers[0].send(3, b"PEER_INPUT")
                until(peers, lambda: all("PEER_RESPONSIVE" in peer.latest[1] for peer in peers),
                      "another pane/client stalled with synchronized paint", timeout=.12)
                assert all("PARTIAL_FIRST" not in peer.latest[0] for peer in peers), "intermediate screen escaped hold"
                if not hung:
                    # Resize during the hold and attach another viewer. Neither
                    # geometry nor a missing baseline should publish a partial paint.
                    peers[0].send(4, struct.pack("!HH", 14, 72))
                    peers.append(Peer(endpoint, library, True))
                    for _ in range(4):
                        for peer in peers:
                            peer.pump()
                        time.sleep(.005)
                    assert all("PARTIAL_FIRST" not in peer.latest[0] for peer in peers)
                    (folder / "finish").touch()
                    until(peers, lambda: all("COMPLETE_LAST" in peer.latest[0] for peer in peers),
                          "completed synchronized viewport was not delivered")
                    for peer in peers:
                        partial = [text for _, pane, text in peer.states if pane == 0 and "PARTIAL_FIRST" in text]
                        assert partial and all("COMPLETE_LAST" in text for text in partial), partial
                    print("PASS synchronized repaint: marker-first paint remains hidden until end; resize/late attach/replies/input/other pane remain live")
                else:
                    until(peers, lambda: all("PARTIAL_FIRST" in peer.latest[0] for peer in peers),
                          "repeated sets extended the hung-block deadline", timeout=.4)
                    shown = min(timestamp for peer in peers for timestamp, pane, text in peer.states
                                if pane == 0 and "PARTIAL_FIRST" in text)
                    elapsed = shown - started
                    assert .16 <= elapsed < .4, elapsed
                    if pairs:
                        # Each write ends inside the following block. The
                        # server sees no inactive final parser state in any
                        # feed, yet a continuous producer must not freeze it.
                        until(peers, lambda: time.monotonic() - started > .55,
                              "paired block stream did not continue", timeout=.6)
                    (folder / "advance").touch()
                    until(peers, lambda: all("POST_TIMEOUT" in peer.latest[0] for peer in peers),
                          "output stopped again after hold expired", timeout=.3 if pairs else .15)
                    assert not (folder / "finished").exists()
                    (folder / "finish").touch()
                    until(peers, lambda: all("COMPLETE_LAST" in peer.latest[0] for peer in peers),
                          "end after expired hold did not update viewport")
                    variant = "continuous end/begin stream" if pairs else "hung synchronized block"
                    print(f"PASS {variant}: begins cannot extend 200ms withholding ({elapsed * 1000:.1f}ms); subsequent output continues until end")
            finally:
                for peer in peers:
                    peer.close()
                for role in range(2):
                    stop_owned_child(folder / f"pid-{role}")
                stop_process(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, default=ROOT / "build/kmx-serve")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    args = parser.parse_args()
    library = load_library(args.library.resolve())
    run(args.server.resolve(), library)
    run(args.server.resolve(), library, True)
    run(args.server.resolve(), library, True, True)


if __name__ == "__main__":
    main()
