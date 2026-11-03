#!/usr/bin/env python3
"""Open a CVSD SCO audio link to a connected headset and hold it open.

The headset only treats Bluetooth as its active audio source while an audio
link is up, so this lets hfp_ag.py see what it sends in that state. Writes
silence so the link has traffic; incoming mic audio is counted and dropped.

  python3 sco.py AA:BB:CC:DD:EE:FF [seconds]
"""
import select
import socket
import sys
import time

addr = sys.argv[1]
hold = float(sys.argv[2]) if len(sys.argv) > 2 else 60

s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_SCO)
s.connect(addr)
print(f"{time.strftime('%H:%M:%S')} SCO up to {addr}, holding {hold:.0f} s", flush=True)
mtu, rx, end = 48, 0, time.time() + hold
silence = bytes(mtu)
while time.time() < end:
    r, w, _ = select.select([s], [s], [], 0.1)
    if r:
        data = s.recv(1024)
        if not data:
            break
        rx += len(data)
        mtu = len(data)
        silence = bytes(mtu)
    if w:
        s.send(silence)
    time.sleep(mtu / 16000)  # 8 kHz, 16-bit mono
s.close()
print(f"{time.strftime('%H:%M:%S')} SCO closed, {rx} bytes of mic audio received", flush=True)
