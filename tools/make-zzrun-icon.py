#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Workbench icon for ZZRun.

Pointers in the file are offsets from the start, which is what
icon.library adds the load address to. Tooltypes are the v0 set from
the app-runtime contract, plus APP and ARGS so a tool icon can name
the ELF and its arguments.

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


def image_struct(data_off):
    return struct.pack(
        ">hhhhhIBBI",
        0, 0, WIDTH, HEIGHT, DEPTH,
        data_off,
        (1 << DEPTH) - 1,
        0,
        0,
    )


def gadget(render_off, select_off):
    # 2-byte Amiga alignment: no pad between ga_GadgetType and ga_GadgetRender.
    return struct.pack(
        ">IhhhhHHHIIIIIhI",
        0,
        0, 0,
        WIDTH, HEIGHT,
        GFLG_GADGIMAGE,
        GACT_RELVERIFY,
        GTYP_BOOLGADGET,
        render_off,
        select_off,
        0,
        0,
        0,
        0,
        0,
    )


def disk_object(render_off, select_off, tools_off):
    body = struct.pack(">HH", 0xE310, 1)
    body += gadget(render_off, select_off)
    body += struct.pack(">BB", WB_TOOL, 0)
    body += struct.pack(">I", 0)          # do_DefaultTool
    body += struct.pack(">I", tools_off)
    body += struct.pack(">II", NO_ICON_POSITION, NO_ICON_POSITION)
    body += struct.pack(">I", 0)          # do_DrawerData
    body += struct.pack(">I", 0)          # do_ToolWindow
    body += struct.pack(">I", STACK)
    if len(body) != 78:
        raise SystemExit("DiskObject is %d bytes, expected 78" % len(body))
    return body


def build():
    normal = to_planes(draw(False))
    selected = to_planes(draw(True))
    image_len = 20
    # DiskObject, two images, two plane blobs, then the tooltype table.
    normal_image = 78
    normal_planes = normal_image + image_len
    select_image = normal_planes + len(normal)
    select_planes = select_image + image_len
    tools_off = select_planes + len(selected)
    strings = [(text + "\0").encode("ascii") for text in TOOLTYPES]
    table_bytes = (len(strings) + 1) * 4
    string_at = tools_off + table_bytes
    pointers = []
    blob = b""
    cursor = string_at
    for raw in strings:
        pointers.append(cursor)
        blob += raw
        cursor += len(raw)
    table = b"".join(struct.pack(">I", off) for off in pointers)
    table += struct.pack(">I", 0)

    out = disk_object(normal_image, select_image, tools_off)
    out += image_struct(normal_planes)
    out += normal
    out += image_struct(select_planes)
    out += selected
    out += table
    out += blob
    if out[tools_off:tools_off + 4] != struct.pack(">I", string_at):
        raise SystemExit("tooltype table was not written at its offset")
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
