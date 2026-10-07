#!/usr/bin/env python3
"""Real-process coding-session benchmark; JSON results use framed KMX bytes only.

Build: make build/kmx-serve build/libkilix-mux.so
Run: python3 tools/coding_bench.py --samples 50
Remote: add --remote SSH_HOST --remote-root /path/to/checkout
Delay/rate shaping is userspace stream scheduling, NOT TCP packet-loss testing.
"""
import argparse
import ctypes as C
import json
import math
import os
from pathlib import Path
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
WORKLOAD = Path(__file__).with_name("coding_workload.py")


class Color(C.Structure):
    _fields_ = [(name, C.c_uint8) for name in ("kind", "r", "g", "b")]


class Cell(C.Structure):
    _fields_ = [("chars", C.c_uint32 * 6), ("attrs", C.c_uint16),
                ("width", C.c_uint8), ("underline", C.c_uint8),
                ("fg", Color), ("bg", Color)]


class Grid(C.Structure):
    _fields_ = [(name, C.c_int) for name in ("rows", "cols", "cursor_row", "cursor_col")]
    _fields_ += [("cursor_visible", C.c_bool), ("cells", C.POINTER(Cell))]


class Receiver:
    def __init__(self, library, rows, cols):
        self.lib = library
        self.handle = C.c_void_p()
        self.check(library.kmx_receiver_create(C.byref(self.handle), rows, cols))

    @staticmethod
    def check(result):
        if result:
            raise RuntimeError(f"native receiver failed: kmx_result={result}")

    def apply(self, payload):
        sequence = C.c_uint64()
        self.check(self.lib.kmx_receiver_apply(self.handle, payload, len(payload), C.byref(sequence)))
        return sequence.value

    def status(self):
        grid = self.lib.kmx_receiver_grid(self.handle).contents
        text = "".join(chr(grid.cells[col].chars[0] or 32) for col in range(grid.cols))
        parts = text.split()
        if len(parts) >= 4 and parts[0] == "CONFIRMED" and parts[2] == "TICK":
            return int(parts[1]), int(parts[3])
        return None

    def validate(self, sequence, tick):
        grid = self.lib.kmx_receiver_grid(self.handle).contents
        expected = [f"CONFIRMED {sequence:08d} TICK {tick:08d}".ljust(grid.cols)[:grid.cols]]
        for row in range(2, grid.rows):
            value = (sequence * 17 + tick * 13 + row) % 100000
            expected.append(f"{row:3d}  def synthetic_{row:02d}(value): return value + {value:05d}".ljust(grid.cols)[:grid.cols])
        expected.append(f"synthetic.py | seq={sequence} | tick={tick}".ljust(grid.cols)[:grid.cols])
        for row, line in enumerate(expected):
            actual = "".join(chr(grid.cells[row * grid.cols + col].chars[0] or 32) for col in range(grid.cols))
            if actual != line:
                raise RuntimeError(f"decoded screen row {row + 1} differs: {actual!r} != {line!r}")

    def close(self):
        if self.handle:
            self.lib.kmx_receiver_free(self.handle)
            self.handle = C.c_void_p()


def load_library(path):
    lib = C.CDLL(str(path))
    lib.kmx_receiver_create.argtypes = [C.POINTER(C.c_void_p), C.c_int, C.c_int]
    lib.kmx_receiver_create.restype = C.c_int
    lib.kmx_receiver_apply.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_uint64)]
    lib.kmx_receiver_apply.restype = C.c_int
    lib.kmx_receiver_grid.argtypes = [C.c_void_p]
    lib.kmx_receiver_grid.restype = C.POINTER(Grid)
    lib.kmx_receiver_free.argtypes = [C.c_void_p]
    return lib


def frame(kind, payload):
    length = len(payload) + 1
    prefix = bytearray()
    while length >= 128:
        prefix.append((length & 127) | 128)
        length >>= 7
    return bytes(prefix) + bytes([length, kind]) + payload


def next_frame(data):
    length = shift = 0
    for pos, byte in enumerate(data):
        length |= (byte & 127) << shift
        if not byte & 128:
            if length < 1 or length > 8 * 1024 * 1024 + 1:
                raise RuntimeError("invalid KMX frame length")
            end = pos + 1 + length
            if len(data) < end:
                return None
            return data[pos + 1], bytes(data[pos + 2:end]), end
        shift += 7
        if shift > 63:
            raise RuntimeError("invalid frame varint")
    return None


def stop_process(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def stop_owned_child(pid_file):
    # pidfd pins the identity before checking /proc; a recycled PID is never signalled.
    if not pid_file.exists():
        return
    descriptor = None
    try:
        recorded = json.loads(pid_file.read_text())
        descriptor = os.pidfd_open(recorded["pid"])
        fields = Path(f"/proc/{recorded['pid']}/stat").read_text().rsplit(")", 1)[1].split()
        if int(fields[19]) == recorded["starttime"] and fields[0] != "Z":
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
    except (OSError, ValueError, KeyError):
        pass
    finally:
        if descriptor is not None:
            os.close(descriptor)


# The SSH supervisor keeps stdin open as a lease. EOF or a signal cleans up.
# Child has its own forkpty session; explicitly reap/terminate its recorded PID.
REMOTE_SUPERVISOR = r'''
import json,os,signal,subprocess,sys,tempfile,time
from pathlib import Path
root,rows,cols,source=sys.argv[1:]
process=None
with tempfile.TemporaryDirectory(prefix="kmx-coding-") as directory:
    folder=Path(directory); pid=folder/"child.pid"; script=folder/"workload.py"
    script.write_text(source); endpoint=str(folder/"session.sock")
    def interrupted(*_): raise SystemExit(1)
    signal.signal(signal.SIGTERM,interrupted); signal.signal(signal.SIGHUP,interrupted)
    try:
        with open(folder/"server.log","wb") as log:
            process=subprocess.Popen([root+"/build/kmx-serve","--socket",endpoint,
                "--rows",rows,"--cols",cols,"--","python3",str(script),
                "--rows",rows,"--cols",cols,"--pid-file",str(pid)],stdout=log,stderr=log)
            deadline=time.monotonic()+8
            while not Path(endpoint).exists():
                if process.poll() is not None or time.monotonic()>deadline:
                    raise RuntimeError((folder/"server.log").read_text())
                time.sleep(.01)
            print(json.dumps({"socket":endpoint,"pid":process.pid}),flush=True)
            while sys.stdin.buffer.read(4096): pass
    finally:
        # Startup may be interrupted after the socket exists but before the child records identity.
        startup_deadline=time.monotonic()+.5
        while not pid.exists() and process is not None and process.poll() is None and time.monotonic()<startup_deadline:
            time.sleep(.01)
        descriptor=None
        try:
            if pid.exists():
                recorded=json.loads(pid.read_text()); descriptor=os.pidfd_open(recorded["pid"])
                fields=Path("/proc/%d/stat"%recorded["pid"]).read_text().rsplit(")",1)[1].split()
                if int(fields[19])==recorded["starttime"] and fields[0]!="Z":
                    signal.pidfd_send_signal(descriptor,signal.SIGTERM)
        except (OSError,ValueError,KeyError): pass
        finally:
            if descriptor is not None: os.close(descriptor)
        if process is not None:
            # Give the server time to reap its child and exit naturally first.
            try: process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.terminate()
                try: process.wait(timeout=2)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
'''

REMOTE_PROXY = r'''
import os,select,socket,sys
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
try:
    while True:
        ready,_,_=select.select([0,s],[],[])
        if 0 in ready:
            data=os.read(0,65536)
            if not data: break
            s.sendall(data)
        if s in ready:
            data=s.recv(65536)
            if not data: break
            view=memoryview(data)
            while view:
                written=os.write(1,view); view=view[written:]
finally: s.close()
'''


def ssh_python(host, source, *args):
    # SSH invokes a shell remotely, so shell-quote every remote argument.
    command = shlex.join(["python3", "-u", "-c", source, *map(str, args)])
    return subprocess.Popen(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, command],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class Session:
    def __init__(self, args):
        self.args = args
        # Retain the benchmark controller's ledger across transport replacement.
        self.input_key = os.urandom(16)
        self.input_epoch = bytes(16)
        self.input_sequence = self.input_accepted = 0
        self.input_pending = {}
        self.temp = tempfile.TemporaryDirectory(prefix="kmx-coding-")
        self.folder = Path(self.temp.name)
        self.process = None
        self.log = None
        try:
            if args.remote:
                self.process = ssh_python(args.remote, REMOTE_SUPERVISOR, args.remote_root,
                                          args.rows, args.cols, WORKLOAD.read_text())
                if not select.select([self.process.stdout], [], [], 15)[0]:
                    raise RuntimeError("remote supervisor did not become ready within 15 seconds")
                line = self.process.stdout.readline()
                if not line:
                    raise RuntimeError(self.process.stderr.read().decode())
                self.endpoint = json.loads(line)["socket"]
            else:
                self.endpoint = str(self.folder / "session.sock")
                self.log = open(self.folder / "server.log", "wb")
                self.process = subprocess.Popen([str(args.server), "--socket", self.endpoint,
                    "--rows", str(args.rows), "--cols", str(args.cols), "--", sys.executable,
                    str(WORKLOAD), "--rows", str(args.rows), "--cols", str(args.cols),
                    "--pid-file", str(self.folder / "child.pid")], stdout=self.log, stderr=self.log)
                deadline = time.monotonic() + 8
                while not Path(self.endpoint).exists():
                    if self.process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError((self.folder / "server.log").read_text())
                    time.sleep(.01)
        except BaseException:
            self.close()
            raise

    def connect(self, lib):
        return Client(self, lib)

    def close(self):
        if self.args.remote:
            if self.process and self.process.stdin:
                self.process.stdin.close()
                try:
                    self.process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    stop_process(self.process)
        else:
            pid_file = self.folder / "child.pid"
            startup_deadline = time.monotonic() + .5
            while not pid_file.exists() and self.process and self.process.poll() is None and time.monotonic() < startup_deadline:
                time.sleep(.01)
            stop_owned_child(pid_file)
            if self.process:
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    stop_process(self.process)
        if self.log:
            self.log.close()
        self.temp.cleanup()


class Client:
    def __init__(self, session, library):
        args = session.args
        self.session = session
        self.input_ready = not args.reliable_input
        self.receiver = Receiver(library, args.rows, args.cols)
        self.delay = args.delay_ms / 1000
        self.rate = args.rate_bytes
        self.tx = self.rx = self.cell_messages = 0
        self.tx_frame_id = self.tx_completed = 0
        self.tx_due = self.rx_due = 0.0
        self.tx_queue = []
        self.rx_queue = []
        self.pending = bytearray()
        self.proxy = self.sock = None
        try:
            if args.remote:
                self.proxy = ssh_python(args.remote, REMOTE_PROXY, session.endpoint)
                self.read_fd = self.proxy.stdout.fileno()
                self.write_fd = self.proxy.stdin.fileno()
            else:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(session.endpoint)
                self.read_fd = self.write_fd = self.sock.fileno()
            os.set_blocking(self.read_fd, False)
            os.set_blocking(self.write_fd, False)
            self.send(1, struct.pack(">HHB", args.rows, args.cols, 0))
            if args.reliable_input:
                self.send(15, b"\x01\0\0\0" + session.input_epoch + session.input_key +
                          struct.pack("!Q", session.input_accepted))
                deadline = time.monotonic() + args.timeout
                while not self.input_ready:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("reliable input selection timed out")
                    self.pump(.001)
        except BaseException:
            self.close()
            raise

    def send(self, kind, payload):
        if kind == 3 and self.session.args.reliable_input:
            if not self.input_ready or not 1 <= len(payload) <= 32768:
                raise RuntimeError("benchmark input requires READY and a bounded payload")
            self.session.input_sequence += 1
            sequence = self.session.input_sequence
            payload = b"\x01\0\0\0" + struct.pack("!Q", sequence) + payload
            self.session.input_pending[sequence] = payload
            kind = 17
        wire = frame(kind, payload)
        now = time.monotonic()
        self.tx_due = max(now + self.delay, self.tx_due) + (len(wire) / self.rate if self.rate else 0)
        self.tx_frame_id += 1
        self.tx_queue.append([self.tx_due, wire, self.tx_frame_id])
        return now

    def accept_input(self, accepted):
        session = self.session
        if not session.input_accepted <= accepted <= session.input_sequence:
            raise RuntimeError("invalid input acceptance counter")
        session.input_accepted = accepted
        session.input_pending = {seq: data for seq, data in session.input_pending.items() if seq > accepted}

    def pump(self, timeout):
        end = time.monotonic() + timeout
        while True:
            now = time.monotonic()
            while self.rx_queue and self.rx_queue[0][0] <= now:
                _, kind, payload = self.rx_queue.pop(0)
                if kind == 2:
                    if not payload or payload[0] != 0:
                        raise RuntimeError("benchmark expects exactly one pane")
                    sequence = self.receiver.apply(payload[1:])
                    self.cell_messages += 1
                    self.send(5, bytes([0]) + struct.pack(">Q", sequence))
                elif kind == 6:
                    raise RuntimeError("workload exited before benchmark completed")
                elif kind == 16 and self.session.args.reliable_input:
                    session = self.session
                    if (self.input_ready or len(payload) != 48 or payload[:4] != b"\x01\0\0\0" or
                            payload[20:36] != session.input_key or payload[4:20] == bytes(16) or
                            (session.input_epoch != bytes(16) and payload[4:20] != session.input_epoch)):
                        raise RuntimeError("server refused reliable input or returned invalid STATE")
                    self.accept_input(struct.unpack("!Q", payload[36:44])[0])
                    session.input_epoch = payload[4:20]
                    self.input_ready = True
                    for data in session.input_pending.values():
                        self.send(17, data)
                elif kind == 18 and self.session.args.reliable_input:
                    if not self.input_ready or len(payload) != 12 or payload[:4] != b"\x01\0\0\0":
                        raise RuntimeError("invalid input ACK")
                    self.accept_input(struct.unpack("!Q", payload[4:])[0])
            if now >= end:
                return
            next_due = min([end] + [q[0][0] for q in (self.tx_queue, self.rx_queue) if q])
            write_ready = self.tx_queue and self.tx_queue[0][0] <= now
            readable, writable, _ = select.select([self.read_fd], [self.write_fd] if write_ready else [], [],
                                                   max(0, min(.05, next_due - now)) if not write_ready else 0.01)
            if writable:
                try:
                    sent = os.write(self.write_fd, self.tx_queue[0][1])
                except BlockingIOError:
                    sent = 0
                self.tx += sent
                self.tx_queue[0][1] = self.tx_queue[0][1][sent:]
                if not self.tx_queue[0][1]:
                    self.tx_completed = self.tx_queue.pop(0)[2]
            if readable:
                data = os.read(self.read_fd, 65536)
                if not data:
                    raise RuntimeError("KMX transport closed")
                self.rx += len(data)
                self.pending.extend(data)
                while True:
                    parsed = next_frame(self.pending)
                    if parsed is None:
                        break
                    kind, payload, length = parsed
                    del self.pending[:length]
                    self.rx_due = max(time.monotonic() + self.delay, self.rx_due) + (length / self.rate if self.rate else 0)
                    self.rx_queue.append((self.rx_due, kind, payload))

    def wait_status(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while True:
            status = self.receiver.status()
            if status is not None and predicate(*status):
                return time.monotonic(), status
            if time.monotonic() >= deadline:
                raise TimeoutError(f"decoded screen confirmation timed out; last status={status}")
            self.pump(min(.001, max(0, deadline - time.monotonic())))

    def close(self):
        if self.sock:
            self.sock.close()
        if self.proxy:
            self.proxy.stdin.close()
            self.proxy.stdin = None
            # EOF follows all bytes already written to SSH's pipe. Let the
            # remote proxy send those bytes before it closes its socket;
            # terminating SSH here can discard the final recovery command.
            try:
                tail, _ = self.proxy.communicate(timeout=3)
                self.rx += len(tail)
            except subprocess.TimeoutExpired:
                stop_process(self.proxy)
        self.receiver.close()


def percentiles(values):
    ordered = sorted(values)
    return {f"p{percentile}_ms": ordered[max(0, math.ceil(len(ordered) * percentile / 100) - 1)]
            for percentile in (50, 95, 99)}


def run(args):
    lib = load_library(args.library)
    session = Session(args)
    client = None
    total_tx = total_rx = 0
    try:
        client = session.connect(lib)
        client.wait_status(lambda seq, tick: seq == 0 and tick == 0, args.timeout)
        latencies = []
        for sequence in range(1, args.samples + 1):
            sent = client.send(3, f"EDIT {sequence}\n".encode())
            confirmed, _ = client.wait_status(lambda seq, tick: seq == sequence, args.timeout)
            latencies.append((confirmed - sent) * 1000)
            client.receiver.validate(sequence, 0)
        # Flush the last acknowledgement before measuring an idle interval.
        client.pump(max(.5, args.delay_ms / 1000 * 3))
        idle_tx, idle_rx = client.tx, client.rx
        client.pump(args.idle_seconds)
        idle = {"seconds": args.idle_seconds, "tx_bytes": client.tx - idle_tx, "rx_bytes": client.rx - idle_rx}
        burst_sequence = args.samples + 1
        burst_start = client.send(3, f"BURST {burst_sequence} {args.burst_frames} {args.burst_pause}\n".encode())
        burst_tx, burst_rx, burst_cells = client.tx, client.rx, client.cell_messages
        finished, (_, tick) = client.wait_status(lambda seq, tick: seq == burst_sequence and tick == args.burst_frames,
                                                max(args.timeout, args.burst_frames * args.burst_pause + args.timeout))
        client.receiver.validate(burst_sequence, tick)
        burst_elapsed = finished - burst_start
        burst = {"generated_repaints": args.burst_frames, "decoded_cell_messages": client.cell_messages - burst_cells,
                 "seconds": burst_elapsed, "generated_repaints_per_second": args.burst_frames / burst_elapsed,
                 "tx_bytes": client.tx - burst_tx, "rx_bytes": client.rx - burst_rx,
                 "rx_kmx_bytes_per_second": (client.rx - burst_rx) / burst_elapsed}
        # Trigger activity, detach during it, and recover its final state on a fresh native receiver.
        recovery_sequence = args.samples + 2
        client.send(3, f"BURST {recovery_sequence} {args.reconnect_frames} 0.005\n".encode())
        detach_input = client.tx_frame_id
        flush_deadline = time.monotonic() + args.timeout
        while client.tx_completed < detach_input:
            if time.monotonic() > flush_deadline:
                raise TimeoutError("input did not leave client before detach")
            client.pump(.001)
        client.close()
        total_tx += client.tx
        total_rx += client.rx
        client = None
        time.sleep(args.reconnect_gap)
        reconnect_start = time.monotonic()
        client = session.connect(lib)
        recovered, status = client.wait_status(lambda seq, tick: seq == recovery_sequence and
                                               tick == args.burst_frames + args.reconnect_frames, args.timeout)
        client.receiver.validate(*status)
        recovery = {"gap_seconds": args.reconnect_gap, "recovery_ms": (recovered - reconnect_start) * 1000,
                    "confirmed_sequence": status[0], "confirmed_tick": status[1],
                    "tx_bytes": client.tx, "rx_bytes": client.rx}
        # Confirm resumed input reaches the child, not only viewport recovery.
        if args.reliable_input:
            client.send(3, f"EDIT {recovery_sequence + 1}\n".encode())
            client.wait_status(lambda seq, tick: seq == recovery_sequence + 1, args.timeout)
            client.receiver.validate(recovery_sequence + 1, status[1])
            deadline = time.monotonic() + args.timeout
            while session.input_pending:
                if time.monotonic() >= deadline:
                    raise TimeoutError("final reliable input acknowledgement timed out")
                client.pump(.001)
        total_tx += client.tx
        total_rx += client.rx
        return {"schema": 1, "verdict": "pass", "transport": "SSH byte proxy to remote Unix socket" if args.remote else "local Unix socket",
                "remote": args.remote, "remote_root": args.remote_root if args.remote else None,
                "input_protocol": "acknowledged-v1" if args.reliable_input else "legacy",
                "input_accepted": session.input_accepted if args.reliable_input else None,
                "screen": {"rows": args.rows, "cols": args.cols},
                "byte_scope": "actual framed KMX stream bytes; excludes SSH, TCP, IP, Ethernet overhead",
                "latency_scope": "input enqueue on client to corresponding confirmation in native decoded remote screen; no local prediction",
                "shaping": {"one_way_delay_ms": args.delay_ms, "per_direction_rate_bytes_per_second": args.rate_bytes,
                            "method": "userspace framed stream scheduling; not TCP packet-loss qualification"},
                "input_latency": {"samples": args.samples, **percentiles(latencies), "max_ms": max(latencies)},
                "idle": idle, "burst": burst, "reconnect": recovery,
                "total": {"tx_bytes": total_tx, "rx_bytes": total_rx}}
    finally:
        if client:
            client.close()
        session.close()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", type=Path, default=ROOT / "build/kmx-serve")
    parser.add_argument("--library", type=Path, default=ROOT / "build/libkilix-mux.so")
    parser.add_argument("--remote", metavar="SSH_HOST")
    parser.add_argument("--remote-root", metavar="PATH")
    parser.add_argument("--reliable-input", action="store_true",
                        help="measure acknowledged input and retain its ledger across reconnect")
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--cols", type=int, default=100)
    parser.add_argument("--samples", type=int, default=50)
    parser.add_argument("--idle-seconds", type=float, default=1)
    parser.add_argument("--burst-frames", type=int, default=200)
    parser.add_argument("--burst-pause", type=float, default=.002)
    parser.add_argument("--reconnect-frames", type=int, default=40)
    parser.add_argument("--reconnect-gap", type=float, default=.3)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--delay-ms", type=float, default=0)
    parser.add_argument("--rate-bytes", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not (4 <= args.rows <= 2000 and 40 <= args.cols <= 2000 and args.rows * args.cols <= 500000):
        parser.error("screen must be >=4x40 and within KMX grid bounds")
    if min(args.samples, args.burst_frames, args.reconnect_frames) < 1:
        parser.error("samples and frame counts must be positive")
    if min(args.delay_ms, args.rate_bytes, args.burst_pause, args.reconnect_gap, args.idle_seconds) < 0 or args.timeout <= 0:
        parser.error("delays and rates must be nonnegative; timeout must be positive")
    if args.remote and not args.remote_root:
        parser.error("--remote requires --remote-root /path/to/checkout")
    if not all(math.isfinite(value) for value in (args.delay_ms, args.burst_pause, args.reconnect_gap, args.idle_seconds, args.timeout)):
        parser.error("timing values must be finite")
    args.server = args.server.resolve()
    args.library = args.library.resolve()
    return args


def main():
    args = parse_args()
    try:
        result = run(args)
    except Exception as error:
        result = {"schema": 1, "verdict": "fail", "error": str(error)}
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0 if result["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
