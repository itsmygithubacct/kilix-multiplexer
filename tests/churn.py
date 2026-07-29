"""Abandon connections at every stage of the handshake, thousands of times.

This drives the state machine through every abandonment point under ASan and
checks whether anything is freed twice, leaked, or used after release.
"""
import socket, ssl, subprocess, time, os, sys, random, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kmxwire import hello, frame, INPUT, RESIZE, ACK, FOCUS

PORT = int(sys.argv[1]); TOKEN = sys.argv[2]; ROUNDS = int(sys.argv[3])
ADDR = ("127.0.0.1", PORT)
random.seed(20260729)

def raw():
    s = socket.socket(); s.settimeout(3.0); s.connect(ADDR); return s

stages = {n: 0 for n in (
    "connect-and-drop", "partial-tls-hello", "tls-then-drop", "greet-then-drop",
    "greet-and-abandon-midframe", "wrong-token", "full-session", "no-greet-linger")}

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

for i in range(ROUNDS):
    pick = random.choice(list(stages))
    try:
        if pick == "connect-and-drop":
            s = raw(); s.close()
        elif pick == "partial-tls-hello":
            s = raw(); s.sendall(b"\x16\x03\x01\x00"); s.close()
        elif pick == "tls-then-drop":
            s = ctx.wrap_socket(raw()); s.close()
        elif pick == "greet-then-drop":
            s = ctx.wrap_socket(raw()); s.sendall(hello(token=TOKEN)); s.close()
        elif pick == "greet-and-abandon-midframe":
            s = ctx.wrap_socket(raw()); s.sendall(hello(token=TOKEN))
            time.sleep(0.01)
            s.sendall(b"\xff\xff")          # a varint length that never completes
            s.close()
        elif pick == "wrong-token":
            s = ctx.wrap_socket(raw()); s.sendall(hello(token="0"*32)); s.close()
        elif pick == "no-greet-linger":
            s = ctx.wrap_socket(raw()); time.sleep(0.05); s.close()
        else:
            s = ctx.wrap_socket(raw()); s.sendall(hello(token=TOKEN))
            time.sleep(0.05)
            try: s.recv(65536)
            except Exception: pass
            s.sendall(frame(INPUT, b"echo hi\r"))
            s.sendall(frame(FOCUS, b"\x00"))
            s.sendall(frame(ACK, b"\x01"))
            s.close()
        stages[pick] += 1
    except Exception:
        stages[pick] += 1        # a refused connection is a valid outcome here
    # Paced to the server's accept allowance (8/second, burst 16).  An earlier
    # unpaced run had 2968 of 3000 connections refused by the rate limiter and
    # therefore tested almost nothing - the churn has to actually get in.
    time.sleep(0.125)

print("rounds by abandonment point:")
for k, v in sorted(stages.items()):
    print("  %-28s %d" % (k, v))
