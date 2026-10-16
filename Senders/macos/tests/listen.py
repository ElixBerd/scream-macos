#!/usr/bin/env python3
"""Receive Scream packets on a UDP port and check what they carry.

Usage: listen.py PORT SECONDS [BITS FREQ]

Without BITS/FREQ: passes if packets with non-silent audio and a stereo
header arrived. With them: also checks the header's sample width, and that
the decoded audio is a sine of FREQ Hz at about -6 dBFS (tones.py plays one),
which only holds if every byte of the PCM is in the right place.
"""
import socket, sys, time

port, seconds = int(sys.argv[1]), float(sys.argv[2])
want_bits = int(sys.argv[3]) if len(sys.argv) > 3 else None
want_freq = float(sys.argv[4]) if len(sys.argv) > 4 else None

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
s.settimeout(0.5)
end = time.time() + seconds
packets = 0
header = None
left = []
while time.time() < end:
    try:
        data = s.recv(2048)
    except socket.timeout:
        continue
    packets += 1
    header = data[:5]
    width = header[1] // 8
    pcm = data[5:]
    full = 1 << (header[1] - 1)
    for i in range(0, len(pcm) - 2 * width + 1, 2 * width):   # left channel only
        left.append(int.from_bytes(pcm[i:i + width], "little", signed=True) / full)

rate = 0
if header:
    base = 44100 if header[0] & 0x80 else 48000
    rate = base * (header[0] & 0x7f)
loud = [x for x in left if abs(x) > 0.01]
peak = max((abs(x) for x in left), default=0.0)

# Frequency from zero crossings over the longest loud stretch.
freq = 0.0
if rate and left:
    start = next((i for i, x in enumerate(left) if abs(x) > 0.2), None)
    if start is not None:
        seg = left[start:start + rate]   # up to one second
        crossings = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
        freq = crossings * rate / len(seg)

print(f"packets={packets} header={header.hex() if header else None} rate={rate} "
      f"samples={len(left)} loud={len(loud)} peak={peak:.3f} freq={freq:.0f}")
ok = packets > 0 and len(loud) > 0 and header[2] == 2 and header[3:5] == b"\x03\x00"
if want_bits is not None:
    ok = ok and header[1] == want_bits
if want_freq is not None:
    ok = ok and 0.45 < peak < 0.55 and abs(freq - want_freq) < want_freq * 0.05
sys.exit(0 if ok else 1)
