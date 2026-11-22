#!/usr/bin/env python3
"""Stream any video (file, URL, webcam, ...) live to a Holoneo over its WebSocket.

Usage: HOLO_TOKEN=... holo_stream.py INPUT [--host holoneo.local] [--fps 20] [--quality 6] [--loop]

ffmpeg scales the input to fit 320x240 on black and encodes JPEG frames; each frame is sent as
one binary WebSocket message, which the device shows immediately. Needs ffmpeg on PATH.
"""
import argparse, base64, os, socket, struct, subprocess, sys, time

p = argparse.ArgumentParser()
p.add_argument("input")
p.add_argument("--host", default="holoneo.local")
p.add_argument("--fps", type=float, default=20)
p.add_argument("--quality", type=int, default=6, help="ffmpeg -q:v, 2 (best) .. 31")
p.add_argument("--loop", action="store_true")
p.add_argument("--seconds", type=float, default=0, help="stop after this many seconds (0 = until the input ends)")
a = p.parse_args()
token = os.environ.get("HOLO_TOKEN") or sys.exit("set HOLO_TOKEN")

s = socket.create_connection((a.host, 80), timeout=10)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET /ws?token={token} HTTP/1.1\r\nHost: {a.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        sys.exit("handshake failed (wrong token?)")
    resp += chunk
if b" 101 " not in resp.split(b"\r\n")[0]:
    sys.exit("handshake failed: " + resp.split(b"\r\n")[0].decode())


def send_binary(payload):
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x82]) + (bytes([0x80 | n]) if n < 126 else
                           bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else
                           bytes([0x80 | 127]) + struct.pack(">Q", n))
    m = (mask * (n // 4 + 1))[:n]
    masked = (int.from_bytes(payload, "big") ^ int.from_bytes(m, "big")).to_bytes(n, "big")
    s.sendall(hdr + mask + masked)


vf = f"fps={a.fps},scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2"
cmd = ["ffmpeg", "-loglevel", "error"] + (["-stream_loop", "-1"] if a.loop else []) + \
      ["-re", "-i", a.input, "-vf", vf, "-pix_fmt", "yuvj420p", "-q:v", str(a.quality), "-f", "image2pipe", "-c:v", "mjpeg", "-"]
ff = subprocess.Popen(cmd, stdout=subprocess.PIPE)
buf, frames, t0 = b"", 0, None
try:
    while True:
        chunk = ff.stdout.read(65536)
        if not chunk:
            break
        buf += chunk
        while True:  # split the MJPEG stream at end-of-image markers
            end = buf.find(b"\xff\xd9")
            if end < 0:
                break
            frame, buf = buf[:end + 2], buf[end + 2:]
            start = frame.find(b"\xff\xd8")
            if start >= 0:
                send_binary(frame[start:])
                if t0 is None:
                    t0 = time.time()
                frames += 1
                if a.seconds and time.time() - t0 > a.seconds:
                    raise KeyboardInterrupt
                if frames % 50 == 0:
                    print(f"{frames} frames, {(frames - 1) / (time.time() - t0):.1f} fps, last {len(frame) // 1024} KB", flush=True)
except (KeyboardInterrupt, BrokenPipeError, ConnectionError):
    pass
finally:
    ff.kill()
    if t0 and frames > 1:
        print(f"sent {frames} frames, {(frames - 1) / (time.time() - t0):.1f} fps average")
