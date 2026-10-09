#!/usr/bin/env python3
#
# the host side of xv6's nettest. QEMU's user-mode network (slirp)
# shows the host to xv6 as 10.0.2.2, and forwards the host's UDP
# port FWDPORT (see the Makefile) to xv6's port 2000.
#
#   python3 nettest.py echo PORT      echo every UDP packet that
#                                     arrives at PORT back to its sender;
#                                     then run "nettest PORT" in xv6.
#   python3 nettest.py ping FWDPORT N first run "nettest server N" in
#                                     xv6; then send N packets to it,
#                                     one at a time, and time each
#                                     round trip.
#

import socket
import sys
import time


def echo(port, idle=60):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(idle)
    n = 0
    print(f"nettest.py: echoing on port {port}", flush=True)
    try:
        while True:
            data, addr = s.recvfrom(4096)
            s.sendto(data, addr)
            n += 1
    except socket.timeout:
        pass
    print(f"nettest.py: echoed {n} packets, then {idle} s without one", flush=True)


def ping(port, n):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    rtts = []
    for i in range(n):
        msg = b"ping %d" % i
        t0 = time.perf_counter()
        s.sendto(msg, ("127.0.0.1", port))
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            print(f"nettest.py: ping {i}: no reply in 5 s: FAIL")
            sys.exit(1)
        rtts.append(time.perf_counter() - t0)
        if data != msg:
            print(f"nettest.py: ping {i}: wrong reply {data!r}: FAIL")
            sys.exit(1)
    rtts.sort()
    us = lambda x: round(x * 1e6)
    print(f"nettest.py: {n} round trips: OK")
    print(f"nettest.py: round trip in microseconds: min {us(rtts[0])}, "
          f"median {us(rtts[n // 2])}, 99th percentile {us(rtts[n * 99 // 100])}, "
          f"max {us(rtts[-1])}")


if len(sys.argv) >= 3 and sys.argv[1] == "echo":
    echo(int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 60)
elif len(sys.argv) == 4 and sys.argv[1] == "ping":
    ping(int(sys.argv[2]), int(sys.argv[3]))
else:
    print("usage: nettest.py echo PORT [IDLE] | nettest.py ping FWDPORT N")
    sys.exit(1)
