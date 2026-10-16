#!/usr/bin/env python3
"""Receive Scream packets on a UDP port and check they carry real audio.

Usage: listen.py PORT SECONDS
Exits 0 if at least one packet with non-silent PCM and a sane header arrived.
"""
import socket, struct, sys, time

port, seconds = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
s.settimeout(0.5)
end = time.time() + seconds
packets = loud = 0
sizes = set()
header = None
while time.time() < end:
    try:
        data = s.recv(2048)
    except socket.timeout:
        continue
    packets += 1
    header = data[:5]
    sizes.add(len(data) - 5)
    if header[1] == 16:
        pcm = struct.unpack("<%dh" % ((len(data) - 5) // 2), data[5:5 + (len(data) - 5) // 2 * 2])
        if max(abs(x) for x in pcm) > 100:
            loud += 1
print(f"packets={packets} loud={loud} payload_sizes={sorted(sizes)} header={header.hex() if header else None}")
ok = packets > 0 and loud > 0 and header[2] == 2 and header[3:5] == b"\x03\x00"
sys.exit(0 if ok else 1)
