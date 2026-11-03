#!/usr/bin/env python3
"""Send a list of probes to a headset's RFCOMM channel and log what comes back.

Each probe goes out on its own, followed by a quiet wait. If the headset
closes the link, that's logged and the next probe reconnects. Probe sets:

  gaia   Qualcomm/CSR GAIA v1 frames (FF 01 flags len vendor cmd ...)
  dsrl   3M Connected Equipment app commands, unframed and with a few
         plausible framings (session, category 0x17, command, interval)
  text   assorted text: AT, CSR BlueCore commands, help prompts

  python3 spp_probe.py AA:BB:CC:DD:EE:FF [--channel 1] [--set gaia,dsrl,text]
                       [--wait 1.5] [--log FILE]
"""
import argparse
import os
import select
import socket
import struct
import time

QUALCOMM = 0x000A


def gaia(cmd, payload=b"", vendor=QUALCOMM, checksum=False):
    body = struct.pack(">HH", vendor, cmd) + payload
    frame = bytes([0xFF, 0x01, 0x01 if checksum else 0x00, len(payload)]) + body
    if checksum:
        c = 0
        for b in frame:
            c ^= b
        frame += bytes([c])
    return frame


def dsrl(cmd, payload=b"", session=1, category=0x17):
    return bytes([session, category, cmd]) + struct.pack("<h", -1) + payload


def probes(sets):
    out = []
    if "gaia" in sets:
        for name, cmd in [("get API version", 0x0300), ("get RSSI", 0x0301),
                          ("get battery", 0x0302), ("get module ID", 0x0303),
                          ("get app version", 0x0304), ("get power state", 0x0284),
                          ("no-op", 0x0700)]:
            out.append((f"gaia {name}", gaia(cmd)))
            out.append((f"gaia {name} +checksum", gaia(cmd, checksum=True)))
        for vendor in (0x001D, 0x0000, 0xFFFF):
            out.append((f"gaia API version vendor {vendor:04x}", gaia(0x0300, vendor=vendor)))
    if "dsrl" in sets:
        for name, cmd in [("GET_RADIO_STATE", 0xB0), ("GET_PTT_CONFIG_DATA", 0xA2),
                          ("PTT_REQUEST RECONNECT", 0xA1)]:
            payload = b"\x00" if cmd == 0xA1 else b""
            raw = dsrl(cmd, payload)
            out.append((f"dsrl {name}", raw))
            out.append((f"dsrl {name} len-prefixed", bytes([len(raw)]) + raw))
            out.append((f"dsrl {name} in GAIA", gaia(0x1700 | cmd, raw[3:])))
    if "text" in sets:
        for t in ["AT\r", "AT\r\n", "ATI\r", "AT+GMR\r", "AT+CGMI\r", "?\r\n", "help\r\n",
                  "\r\n", "AT+CTKST?\r", "AT+CTKST=?\r", "AT+VOLOUT?\r", "+CTKST: 0,1\r\n"]:
            out.append((f"text {t!r}", t.encode()))
    return out


def show(data):
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    return f"{data.hex(' ')}  |{text}|"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("addr")
    p.add_argument("--channel", type=int, default=1)
    p.add_argument("--set", default="gaia,dsrl,text")
    p.add_argument("--wait", type=float, default=1.5)
    p.add_argument("--log", default=os.path.expanduser(
        f"~/peltor/logs/spp-probe-{time.strftime('%Y%m%d-%H%M%S')}.txt"))
    args = p.parse_args()
    logf = open(args.log, "a")

    def log(line):
        line = f"{time.strftime('%H:%M:%S')} {line}"
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    def drain(sock, wait):
        """Log anything received within `wait` seconds; False if the link closed."""
        end = time.time() + wait
        while (left := end - time.time()) > 0:
            r, _, _ = select.select([sock], [], [], left)
            if not r:
                break
            data = sock.recv(4096)
            if not data:
                return False
            log(f"   << {show(data)}")
        return True

    sock = None
    for name, data in probes(args.set.split(",")):
        if sock is None:
            sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
            sock.connect((args.addr, args.channel))
            log(f"-- connected channel {args.channel}")
            if not drain(sock, args.wait):
                log("-- closed by headset on connect")
                sock = None
                continue
        log(f">> {name}: {show(data)}")
        sock.send(data)
        if not drain(sock, args.wait):
            log("-- closed by headset")
            sock.close()
            sock = None
    if sock:
        sock.close()


if __name__ == "__main__":
    main()
