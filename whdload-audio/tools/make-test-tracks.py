#!/usr/bin/env python3
# Test tracks for the Audio/K option of WHDLoad and the ZZ9000AX driver of
# WHDLoad.Audio (whdload-audio/README.md).
#
# Writes track01.wav .. track09.wav into a directory. Every track is a plain
# PCM WAV file, so the Amiga needs no decoder. The tracks differ so that the
# keys of the test Slave (audio.asm of winstalls) show what the driver does:
#
#   01  10 s    440 Hz, both channels, steady
#   02   5 s    880 Hz, left channel only
#   03   5 s    880 Hz, right channel only
#   04   0.3 s  1 kHz, both channels (loop, count)
#   05   0.04 s 1 kHz click, shorter than the preroll of the card
#   06   4 s    523 Hz, 8 bit mono
#   07   4 s    392 Hz, 16 bit mono
#   08   3 s    440 Hz, 48000 Hz (the driver refuses it, the end is reported
#               at once and nothing plays)
#   09  10 s    330 Hz in five steps of 2 s, each louder than the one before
#
# Usage: make-test-tracks.py [--check] [DIR]     (default DIR: tracks)
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

import math
import struct
import sys
import wave

TRACKS = [
    # number, seconds, rate, channels, bits, kind
    (1, 10.0, 44100, 2, 16, "both"),
    (2, 5.0, 44100, 2, 16, "left"),
    (3, 5.0, 44100, 2, 16, "right"),
    (4, 0.3, 44100, 2, 16, "both1k"),
    (5, 0.04, 44100, 2, 16, "both1k"),
    (6, 4.0, 44100, 1, 8, "mono523"),
    (7, 4.0, 44100, 1, 16, "mono392"),
    (8, 3.0, 48000, 2, 16, "both"),
    (9, 10.0, 44100, 2, 16, "steps"),
]
FREQ = {"both": 440.0, "left": 880.0, "right": 880.0, "both1k": 1000.0,
        "mono523": 523.25, "mono392": 392.0, "steps": 330.0}


def samples(seconds, rate, channels, kind):
    frames = int(round(seconds * rate))
    freq = FREQ[kind]
    out = []
    for i in range(frames):
        amp = 0.5
        if kind == "steps":
            amp = 0.1 + 0.8 * min(int(i / (2.0 * rate)), 4) / 4.0
        if kind == "both1k" and frames < rate // 10:
            amp = 0.5
        s = amp * math.sin(2.0 * math.pi * freq * i / rate)
        left = right = s
        if kind == "left":
            right = 0.0
        elif kind == "right":
            left = 0.0
        out.append((left, right) if channels == 2 else (left,))
    return out


def encode(frames, bits):
    data = bytearray()
    for frame in frames:
        for v in frame:
            if bits == 16:
                data += struct.pack("<h", int(round(v * 32767)))
            else:
                data.append(int(round(v * 127)) + 128)
    return bytes(data)


def write_all(directory):
    import os
    os.makedirs(directory, exist_ok=True)
    for number, seconds, rate, channels, bits, kind in TRACKS:
        path = os.path.join(directory, "track%02d.wav" % number)
        with wave.open(path, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(bits // 8)
            w.setframerate(rate)
            w.writeframes(encode(samples(seconds, rate, channels, kind), bits))
        print(path)


def check(directory):
    import os
    for number, seconds, rate, channels, bits, kind in TRACKS:
        path = os.path.join(directory, "track%02d.wav" % number)
        with wave.open(path, "rb") as w:
            assert w.getnchannels() == channels, path
            assert w.getsampwidth() == bits // 8, path
            assert w.getframerate() == rate, path
            assert w.getnframes() == int(round(seconds * rate)), path
    print("ok")


def main(argv):
    args = [a for a in argv[1:] if a != "--check"]
    directory = args[0] if args else "tracks"
    write_all(directory)
    if "--check" in argv:
        check(directory)


if __name__ == "__main__":
    main(sys.argv)
