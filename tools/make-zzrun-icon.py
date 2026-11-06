#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Workbench icon for ZZRun.

icon.library stores DiskObject pointers as presence markers (1 or 0), then
appends bodies in order: DrawerData, render Image, select Image,
do_DefaultTool, do_ToolTypes, do_ToolWindow. ToolTypes are a longword of
(count + 1) * 4 followed by length-prefixed NUL-terminated strings, which
is what PutDiskObject writes and what Install ZZ9000.info uses. An offset
table of raw C strings is not a ToolType array.

Usage: python3 tools/make-zzrun-icon.py [--check]
"""

import os
import struct
import sys

WIDTH = 32
HEIGHT = 32
DEPTH = 2
WB_TOOL = 3
GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY = 0x0001
GTYP_BOOLGADGET = 0x0001
NO_ICON_POSITION = 0x80000000
STACK = 32768

TARGETS = (
    "ZZRun/ZZRun.info",
    "installer/ZZ9000Installer/Tools/ZZRun.info",
)

TOOLTYPES = (
    "APP=",
    "ROOT=",
    "ABS=0",
    "MODE=",
    "ARGS=",
)


def blank():
    return [[0] * WIDTH for _ in range(HEIGHT)]


def fill_rect(pix, x0, y0, x1, y1, colour):
    for y in range(max(0, y0), min(HEIGHT, y1)):
        for x in range(max(0, x0), min(WIDTH, x1)):
            pix[y][x] = colour


def frame_rect(pix, x0, y0, x1, y1, colour):
    for x in range(x0, x1):
        pix[y0][x] = colour
        pix[y1 - 1][x] = colour
    for y in range(y0, y1):
        pix[y][x0] = colour
        pix[y][x1 - 1] = colour


def draw(selected):
    """Workbench pens: 0 grey, 1 black, 2 white, 3 blue."""
    pix = blank()
    fill_rect(pix, 2, 3, 30, 24, 3)
    frame_rect(pix, 2, 3, 30, 24, 1)
    fill_rect(pix, 4, 5, 28, 22, 1 if not selected else 2)
    # Run mark. Selected swaps the pen so the click still reads.
    ink = 2 if not selected else 1
    for row, half in ((10, 2), (11, 3), (12, 4), (13, 5), (14, 4), (15, 3), (16, 2)):
        fill_rect(pix, 12, row, 12 + half, row + 1, ink)
    fill_rect(pix, 8, 26, 24, 28, 1)
    fill_rect(pix, 14, 28, 18, 30, 1)
    return pix


def to_planes(pix):
    words = (WIDTH + 15) // 16
    row_bytes = words * 2
    out = bytearray()
    for plane in range(DEPTH):
        for y in range(HEIGHT):
            row = bytearray(row_bytes)
            for x in range(WIDTH):
                if (pix[y][x] >> plane) & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            out += row
    return bytes(out)


def image_struct():
    # ImageData is a presence marker. Planes follow this 20-byte struct.
    return struct.pack(
        ">hhhhhIBBI",
        0, 0, WIDTH, HEIGHT, DEPTH,
        1,
        (1 << DEPTH) - 1,
        0,
        0,
    )


def gadget():
    # 2-byte Amiga alignment: no pad between ga_GadgetType and ga_GadgetRender.
    # Render pointers are presence markers, not file offsets.
    return struct.pack(
        ">IhhhhHHHIIIIIhI",
        0,
        0, 0,
        WIDTH, HEIGHT,
        GFLG_GADGIMAGE,
        GACT_RELVERIFY,
        GTYP_BOOLGADGET,
        1,
        1,
        0,
        0,
        0,
        0,
        0,
    )


def disk_object():
    body = struct.pack(">HH", 0xE310, 1)
    body += gadget()
    body += struct.pack(">BB", WB_TOOL, 0)
    body += struct.pack(">I", 0)          # do_DefaultTool
    body += struct.pack(">I", 1)          # do_ToolTypes present
    body += struct.pack(">II", NO_ICON_POSITION, NO_ICON_POSITION)
    body += struct.pack(">I", 0)          # do_DrawerData
    body += struct.pack(">I", 0)          # do_ToolWindow
    body += struct.pack(">I", STACK)
    if len(body) != 78:
        raise SystemExit("DiskObject is %d bytes, expected 78" % len(body))
    return body


def string_body(text):
    raw = text.encode("ascii") + b"\0"
    return struct.pack(">I", len(raw)) + raw


def tooltypes_body(entries):
    # The longword is the in-memory pointer-table size, including the NULL
    # terminator. Each string then carries its own length, including NUL.
    out = struct.pack(">I", (len(entries) + 1) * 4)
    for entry in entries:
        out += string_body(entry)
    return out


def build():
    normal = to_planes(draw(False))
    selected = to_planes(draw(True))
    tools = tooltypes_body(TOOLTYPES)
    out = disk_object()
    out += image_struct() + normal
    out += image_struct() + selected
    out += tools
    if struct.unpack_from(">I", out, 54)[0] != 1:
        raise SystemExit("do_ToolTypes is not a presence marker")
    tools_at = 78 + 20 + len(normal) + 20 + len(selected)
    if struct.unpack_from(">I", out, tools_at)[0] != (len(TOOLTYPES) + 1) * 4:
        raise SystemExit("tooltype table size was not written")
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    check = "--check" in sys.argv
    data = build()
    status = 0
    for rel in TARGETS:
        path = os.path.join(root, rel)
        if check:
            existing = open(path, "rb").read() if os.path.exists(path) else None
            if existing != data:
                print("STALE %s (re-run %s)" % (rel, os.path.basename(__file__)))
                status = 1
            else:
                print("ok    %s" % rel)
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)
            print("wrote %s (%d bytes)" % (rel, len(data)))
    return status


if __name__ == "__main__":
    sys.exit(main())
