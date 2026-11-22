#!/usr/bin/env python3
"""Capture the Holoneo's serial log (stdlib only, no pyserial needed).

Usage:
    serial_log.py [--port /dev/ttyUSB0] [--baud 115200] [--seconds 15] [--reset] [--out boot.log]

--reset pulses the ESP32 auto-reset lines (RTS -> EN, DTR -> IO0) so the board
reboots into its normal firmware and you capture the full boot log.
"""
import argparse, fcntl, os, select, struct, sys, termios, time

BAUDS = {9600: termios.B9600, 57600: termios.B57600, 115200: termios.B115200,
         230400: termios.B230400, 460800: termios.B460800, 921600: termios.B921600}


def open_port(dev, baud):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    if baud not in BAUDS:
        sys.exit(f"unsupported baud {baud}; use one of {sorted(BAUDS)}")
    a[4] = a[5] = BAUDS[baud]
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def set_lines(fd, on=0, off=0):
    if on:
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", on))
    if off:
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", off))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="/dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=15)
    p.add_argument("--reset", action="store_true", help="reboot the board first")
    p.add_argument("--out", help="also save raw bytes to this file")
    args = p.parse_args()

    fd = open_port(args.port, args.baud)
    if args.reset:
        dtr, rts = termios.TIOCM_DTR, termios.TIOCM_RTS
        set_lines(fd, on=rts, off=dtr)   # EN low = reset, IO0 high = normal boot
        time.sleep(0.15)
        set_lines(fd, off=rts | dtr)
    termios.tcflush(fd, termios.TCIOFLUSH)

    data = b""
    end = time.time() + args.seconds
    while time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            data += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace").replace("\r", ""))
            sys.stdout.flush()
    os.close(fd)
    if args.out:
        open(args.out, "wb").write(data)


if __name__ == "__main__":
    main()
