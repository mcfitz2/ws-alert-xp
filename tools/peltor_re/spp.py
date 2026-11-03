#!/usr/bin/env python3
"""Connect to a headset's RFCOMM channel and log raw bytes both ways.

Anything written to the FIFO (default ~/peltor/spp.in) is sent as-is;
"\\r", "\\n" and "\\xNN" escapes are expanded, so binary probes work:

  echo 'AT\\r' > ~/peltor/spp.in

  python3 spp.py AA:BB:CC:DD:EE:FF [channel] [--log FILE]
"""
import argparse
import os
import select
import socket
import time

FIFO = os.path.expanduser("~/peltor/spp.in")


def stamp():
    return f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d}"


def show(data):
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    return f"{data.hex(' ')}  |{text}|"


p = argparse.ArgumentParser()
p.add_argument("addr")
p.add_argument("channel", type=int, nargs="?", default=1)
p.add_argument("--log", default=os.path.expanduser(f"~/peltor/logs/spp-{time.strftime('%Y%m%d-%H%M%S')}.txt"))
args = p.parse_args()

logf = open(args.log, "a")


def log(line):
    line = f"{stamp()} {line}"
    print(line, flush=True)
    logf.write(line + "\n")
    logf.flush()


if not os.path.exists(FIFO):
    os.mkfifo(FIFO)
fifo = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)

s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
s.connect((args.addr, args.channel))
log(f"-- connected {args.addr} channel {args.channel}")
while True:
    r, _, _ = select.select([s, fifo], [], [])
    if s in r:
        data = s.recv(4096)
        if not data:
            log("-- closed by headset")
            break
        log(f"<< {show(data)}")
    if fifo in r:
        raw = os.read(fifo, 4096).decode(errors="replace").rstrip("\n")
        data = raw.encode().decode("unicode_escape").encode("latin-1")
        log(f">> {show(data)}")
        s.send(data)
