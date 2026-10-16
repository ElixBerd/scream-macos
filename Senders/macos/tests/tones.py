#!/usr/bin/env python3
"""Write a stereo 48 kHz WAV with a sine at -6 dBFS. Usage: tones.py OUT.wav FREQ SECONDS"""
import math, struct, sys, wave

out, freq, seconds = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rate = 48000
with wave.open(out, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(3)   # 24 bit, so the source itself adds no 16 bit noise
    w.setframerate(rate)
    frames = bytearray()
    for n in range(int(rate * seconds)):
        v = int(round(0.5 * math.sin(2 * math.pi * freq * n / rate) * 8388607))
        b = v.to_bytes(3, "little", signed=True)
        frames += b + b
    w.writeframes(bytes(frames))
