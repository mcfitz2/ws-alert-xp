#!/usr/bin/env python3
"""Minimal HFP/HSP Audio Gateway for probing the Peltor push-to-talk protocol.

Registers an AG profile with BlueZ, answers the headset's service level
connection handshake, and logs every AT command the headset sends. Lines
written to the FIFO (default ~/peltor/ag.in) are sent to the headset as
unsolicited results, so commands can be tried by hand:

  echo '+CIEV: 2,1' > ~/peltor/ag.in

Needs PipeWire's Bluetooth monitor off (wp-bt.sh off), because only one
process can own the AG UUID.

  python3 hfp_ag.py [--hsp] [--connect AA:BB:CC:DD:EE:FF] [--unknown ok|error]
                    [--brsf N] [--log FILE]
"""
import argparse
import os
import socket
import sys
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb"
HFP_HF = "0000111e-0000-1000-8000-00805f9b34fb"
HSP_AG = "00001112-0000-1000-8000-00805f9b34fb"
HSP_HS = "00001108-0000-1000-8000-00805f9b34fb"
PROFILE_PATH = "/peltor/ag"
FIFO = os.path.expanduser("~/peltor/ag.in")

CIND_TEST = ('+CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0-3)),'
             '("callheld",(0-2)),("signal",(0-5)),("roam",(0,1)),("battchg",(0-5))')

args = None
logf = None
links = {}  # fd -> socket


def log(direction, text):
    line = f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {direction} {text}"
    print(line, flush=True)
    if logf:
        logf.write(line + "\n")
        logf.flush()


def send(sock, text):
    log(">>", text)
    sock.send(f"\r\n{text}\r\n".encode())


def reply(cmd):
    """Return the result lines for one AT command (without the final OK/ERROR)."""
    c = cmd.upper()
    for spec in args.resp:
        prefix, _, lines = spec.partition("=>")
        if c.startswith(prefix.upper()):
            return [line for line in lines.split("|") if line]
    if c.startswith("AT+BRSF="):
        return [f"+BRSF: {args.brsf}"]
    if c == "AT+CIND=?":
        return [CIND_TEST]
    if c == "AT+CIND?":
        return ["+CIND: 1,0,0,0,5,0,5"]
    if c == "AT+CHLD=?":
        return ["+CHLD: (0,1,2,3)"]
    if c == "AT+BIND=?":
        return ["+BIND: (1,2)"]
    if c == "AT+BIND?":
        return ["+BIND: 1,1", "+BIND: 2,1"]
    if c == "AT+COPS?":
        return ['+COPS: 0,0,"PELTOR"']
    if c.startswith(("AT+BAC=", "AT+CMER=", "AT+BIND=", "AT+CMEE=", "AT+CLIP=", "AT+CCWA=",
                     "AT+NREC=", "AT+VGS=", "AT+VGM=", "AT+BIA=", "AT+COPS=", "AT+CKPD=",
                     "AT+BIEV=", "AT+BCS=", "AT+BCC", "AT+CLCC", "AT+BTRH?", "AT+XAPL=",
                     "AT+IPHONEACCEV=")):
        return []
    return None


def on_data(fd, cond, buf=[b""]):
    sock = links.get(fd)
    if sock is None or cond & (GLib.IO_HUP | GLib.IO_ERR):
        log("--", "link closed")
        links.pop(fd, None)
        return False
    data = sock.recv(1024)
    if not data:
        log("--", "link closed")
        links.pop(fd, None)
        return False
    buf[0] += data
    while b"\r" in buf[0]:
        raw, buf[0] = buf[0].split(b"\r", 1)
        cmd = raw.strip(b"\n").decode(errors="replace")
        if not cmd:
            continue
        log("<<", cmd)
        lines = reply(cmd)
        if lines is None:
            log("!!", "unknown command")
            send(sock, "OK" if args.unknown == "ok" else "ERROR")
        else:
            for line in lines:
                send(sock, line)
            send(sock, "OK")
            # codec negotiation: the AG picks a codec once the SLC is up
            if args.bcs and cmd.upper().startswith("AT+CMER="):
                send(sock, f"+BCS: {args.bcs}")
        # unsolicited assertion: after a command starting with PREFIX, push LINE
        # (used to answer inside the 2.1 s radio-setup window, e.g. assert the
        # radio is keyed/ready right after the headset sends AT+CTKST=0,1)
        for spec in args.fire:
            prefix, _, extra = spec.partition("=>")
            if extra and cmd.upper().startswith(prefix.upper()):
                send(sock, extra)
    return True


def on_fifo(fd, cond):
    text = os.read(fd, 4096).decode(errors="replace")
    for line in text.splitlines():
        line = line.strip()
        if line:
            for sock in list(links.values()):
                send(sock, line)
    return True


class Profile(dbus.service.Object):
    @dbus.service.method("org.bluez.Profile1", in_signature="oha{sv}", out_signature="")
    def NewConnection(self, device, fd, props):
        fd = fd.take()
        sock = socket.socket(fileno=fd)
        links[fd] = sock
        log("--", f"connected {device} {dict((str(k), str(v)) for k, v in props.items())}")
        GLib.io_add_watch(fd, GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, on_data)

    @dbus.service.method("org.bluez.Profile1", in_signature="o", out_signature="")
    def RequestDisconnection(self, device):
        log("--", f"disconnect requested {device}")
        for sock in links.values():
            sock.close()
        links.clear()

    @dbus.service.method("org.bluez.Profile1", in_signature="", out_signature="")
    def Release(self):
        log("--", "profile released")


def main():
    global args, logf
    p = argparse.ArgumentParser()
    p.add_argument("--hsp", action="store_true", help="act as an HSP AG instead of HFP")
    p.add_argument("--connect", metavar="ADDR", help="connect out to this headset")
    p.add_argument("--unknown", choices=("ok", "error"), default="ok",
                   help="reply to unrecognised commands")
    p.add_argument("--brsf", type=int, default=0, help="AG supported features bitmap")
    p.add_argument("--bcs", type=int, default=0,
                   help="codec to select with +BCS after AT+CMER (1 CVSD, 2 mSBC; 0 off)")
    p.add_argument("--resp", action="append", default=[], metavar="PREFIX=>LINE|LINE",
                   help="reply to commands starting with PREFIX with these lines, then OK "
                        "(empty LINE list: just OK); repeatable, checked first")
    p.add_argument("--fire", action="append", default=[], metavar="PREFIX=>LINE",
                   help="after answering a command starting with PREFIX, push LINE as an "
                        "unsolicited result; repeatable")
    p.add_argument("--name", default="Peltor AG", help="SDP service name")
    p.add_argument("--log", default=os.path.expanduser(f"~/peltor/logs/ag-{time.strftime('%Y%m%d-%H%M%S')}.txt"))
    args = p.parse_args()

    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    logf = open(args.log, "a")
    log("--", f"args {vars(args)}")

    if not os.path.exists(FIFO):
        os.mkfifo(FIFO)
    fifo_fd = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)
    GLib.io_add_watch(fifo_fd, GLib.IO_IN, on_fifo)

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    Profile(bus, PROFILE_PATH)
    uuid, remote = (HSP_AG, HSP_HS) if args.hsp else (HFP_AG, HFP_HF)
    opts = {"Name": args.name, "AutoConnect": True, "RequireAuthentication": True,
            "RequireAuthorization": False}
    if not args.hsp:
        opts["Features"] = dbus.UInt16(0)
    mgr = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.ProfileManager1")
    mgr.RegisterProfile(PROFILE_PATH, uuid, opts)
    log("--", f"registered {'HSP' if args.hsp else 'HFP'} AG; FIFO {FIFO}")

    if args.connect:
        path = "/org/bluez/hci0/dev_" + args.connect.upper().replace(":", "_")
        dev = dbus.Interface(bus.get_object("org.bluez", path), "org.bluez.Device1")
        dev.ConnectProfile(remote, reply_handler=lambda: log("--", "ConnectProfile ok"),
                           error_handler=lambda e: log("!!", f"ConnectProfile failed: {e}"))

    try:
        GLib.MainLoop().run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
