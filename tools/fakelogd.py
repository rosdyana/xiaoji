#!/usr/bin/env python3
"""Print Android log messages sent to a stand-in logd socket.

Usage: fakelogd.py SOCKET_PATH [IDLE_SECONDS]

Android processes write logs as datagrams to /dev/socket/logdw. In the oracle
chroot nothing listens there, so ART's fatal errors vanish. This binds the
socket and prints "P<priority> <tag>: <message>" per datagram until no message
arrives for IDLE_SECONDS (default 10).

Datagram layout (liblog): log id (1 byte), tid (2), realtime (8), then the
payload: priority (1), tag NUL, message NUL.
"""
import os, socket, sys

path = sys.argv[1]
idle = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
try:
    os.unlink(path)
except FileNotFoundError:
    pass
os.makedirs(os.path.dirname(path), exist_ok=True)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
os.chmod(path, 0o666)
s.settimeout(idle)
try:
    while True:
        body = s.recv(65536)[11:]
        if len(body) < 2:
            continue
        parts = body[1:].split(b"\0")
        msg = parts[1].decode(errors="replace") if len(parts) > 1 else ""
        print(f"P{body[0]} {parts[0].decode(errors='replace')}: {msg}", flush=True)
except socket.timeout:
    pass
