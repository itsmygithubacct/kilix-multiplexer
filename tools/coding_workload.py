#!/usr/bin/env python3
"""Deterministic, repo-safe editor-like PTY workload for coding_bench.py."""
import argparse
import json
import os
from pathlib import Path
import sys
import termios
import time
import tty


def paint(sequence, tick, rows, cols, full=True):
    # The fixed first row is validated as decoded cells, not ANSI output.
    chunks = ["\x1b[?2026h\x1b[?25l", "\x1b[2J" if full else ""]
    for row in range(2, rows):
        value = (sequence * 17 + tick * 13 + row) % 100000
        code = f"{row:3d}  def synthetic_{row:02d}(value): return value + {value:05d}"
        chunks.append(f"\x1b[{row};1H\x1b[{'36' if row % 3 else '33'}m"
                      + code.ljust(cols)[:cols] + "\x1b[0m")
    chunks.append(f"\x1b[{rows};1H" + f"synthetic.py | seq={sequence} | tick={tick}".ljust(cols)[:cols])
    # Commit marker LAST: a PTY read can split even one write mid-repaint.
    chunks.append("\x1b[H" + f"CONFIRMED {sequence:08d} TICK {tick:08d}".ljust(cols)[:cols] + "\x1b[?2026l")
    pending = memoryview("".join(chunks).encode())
    while pending:
        written = os.write(1, pending)
        if written <= 0:
            raise RuntimeError("PTY stopped accepting synthetic repaint")
        pending = pending[written:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--cols", type=int, default=100)
    parser.add_argument("--pid-file")
    args = parser.parse_args()
    if args.pid_file:
        pid = Path(args.pid_file)
        temporary_pid = pid.with_suffix(".tmp")
        temporary_pid.write_text(json.dumps({"pid": os.getpid(), "starttime":
            int(Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19])}))
        temporary_pid.replace(pid)
    original = termios.tcgetattr(0)
    tty.setraw(0)
    sequence = tick = 0
    pending = b""
    try:
        paint(sequence, tick, args.rows, args.cols)
        while True:
            data = os.read(0, 4096)
            if not data:
                break
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                fields = line.decode("ascii").split()
                if not fields:
                    continue
                if fields[0] == "EDIT":
                    sequence = int(fields[1])
                    paint(sequence, tick, args.rows, args.cols, full=False)
                elif fields[0] == "BURST":
                    sequence = int(fields[1])
                    count, pause = int(fields[2]), float(fields[3])
                    for _ in range(count):
                        tick += 1
                        paint(sequence, tick, args.rows, args.cols)
                        if pause:
                            time.sleep(pause)
                elif fields[0] == "QUIT":
                    return
    finally:
        termios.tcsetattr(0, termios.TCSANOW, original)


if __name__ == "__main__":
    main()
