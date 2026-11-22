#!/usr/bin/env python3
"""Print live events (gestures) from a Holoneo running the custom firmware.

Usage: HOLO_TOKEN=... holo_events.py [host]      (default host: holoneo.local)
Minimal WebSocket client using only the standard library.
"""
import base64, os, socket, struct, sys

host = sys.argv[1] if len(sys.argv) > 1 else "holoneo.local"
token = os.environ.get("HOLO_TOKEN") or sys.exit("set HOLO_TOKEN (shown on the device: hover your hand over it)")

s = socket.create_connection((host, 80), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET /ws?token={token} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        sys.exit("connection closed during handshake (wrong token?)")
    resp += chunk
if b" 101 " not in resp.split(b"\r\n")[0]:
    sys.exit("handshake failed: " + resp.split(b"\r\n")[0].decode())
print(f"connected to {host}, waiting for events (Ctrl-C to stop)", flush=True)
s.settimeout(None)


def recv_exact(n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


try:
    while True:
        b0, b1 = recv_exact(2)
        ln = b1 & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", recv_exact(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", recv_exact(8))[0]
        payload = recv_exact(ln)
        if b0 & 0x0F == 1:
            print(payload.decode(), flush=True)
        elif b0 & 0x0F == 8:
            break
except (KeyboardInterrupt, ConnectionError):
    pass
