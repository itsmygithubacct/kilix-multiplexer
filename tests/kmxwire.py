"""The kmx wire format, for tests that need to be a peer rather than a client.

Deliberately hand-rolled rather than bound to the library: a test that encodes
its frames with the same code the server decodes them with cannot catch a
framing mistake, only an inconsistency.

The length prefix is a varint, not a fixed-width integer.  Getting that wrong
produces a server that accepts the connection, answers once, and then closes -
which reads exactly like a server bug and is not one.
"""
import struct
def varint(v):
    out=bytearray()
    while True:
        b=v & 0x7f; v >>= 7
        if v: b |= 0x80
        out.append(b)
        if not v: break
    return bytes(out)
def frame(t, payload):
    body = bytes([t]) + payload
    return varint(len(body)) + body
HELLO, CELLS, INPUT, RESIZE, ACK, EXIT, LAYOUT, FOCUS = 1,2,3,4,5,6,7,8
def hello(rows=24, cols=80, role=0, token=None):
    p = bytes([rows>>8, rows&0xff, cols>>8, cols&0xff, role])
    if token: p += token.encode()
    return frame(HELLO, p)
