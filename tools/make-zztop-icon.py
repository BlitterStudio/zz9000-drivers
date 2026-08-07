#!/usr/bin/env python3
# Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the classic AmigaOS icon for ZZTop.

The .info files are committed binaries, so this script exists to make them
reproducible and reviewable: run it and the committed bytes must come back
identical. It emits a plain OS 1.x/3.x DiskObject icon (no NewIcon/GlowIcon
chunk) because that renders on every system ZZTop supports.

The artwork deliberately mirrors the ZZPlay icon's monitor body and stand -
they are the two GUI-facing ZZ9000 tools and should read as a pair - with
the screen showing control sliders instead of a play triangle.

Layout reference: struct DiskObject (78 bytes) followed, in order, by the
render Image, the select Image, and (unused here) the do_DefaultTool,
do_ToolTypes and do_ToolWindow bodies. ZZTop reads no tooltypes, so it has
none: an icon full of inert entries would just invite people to set them.

Usage: python3 tools/make-zztop-icon.py [--check]
"""

import os
import struct
import sys

WIDTH = 46
HEIGHT = 46
DEPTH = 2

WB_TOOL = 3

GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY = 0x0001
GTYP_BOOLGADGET = 0x0001

# Written relative to the repository root.
TARGETS = (
    "ZZTop/ZZTop.info",
    "installer/ZZ9000Installer/Tools/ZZTop.info",
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


def slider(pix, y, knob_x, track, knob):
    """One control row: a thin track with a raised knob sitting on it."""
    fill_rect(pix, 10, y, 36, y + 1, track)
    fill_rect(pix, knob_x, y - 2, knob_x + 4, y + 3, knob)


def draw(selected):
    """Colours are Workbench pens: 0 grey, 1 black, 2 white, 3 blue."""
    pix = blank()
    # Monitor body and screen, matching the ZZPlay icon.
    fill_rect(pix, 3, 5, 43, 34, 3)
    frame_rect(pix, 3, 5, 43, 34, 1)
    fill_rect(pix, 6, 8, 40, 31, 1 if not selected else 2)

    # Three control sliders at different settings - the tool is a control
    # panel, and staggered knobs say that faster than any single symbol.
    track = 3 if not selected else 0
    knob = 2 if not selected else 1
    slider(pix, 13, 14, track, knob)
    slider(pix, 19, 26, track, knob)
    slider(pix, 25, 19, track, knob)

    # Stand.
    fill_rect(pix, 19, 34, 27, 39, 1)
    fill_rect(pix, 12, 39, 34, 42, 3)
    frame_rect(pix, 12, 39, 34, 42, 1)
    return pix


def to_planes(pix):
    """Amiga planar: one bitplane at a time, rows padded to a 16-bit word."""
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
    return struct.pack(
        ">hhhhhIBBI",
        0, 0, WIDTH, HEIGHT, DEPTH,
        1,                          # ImageData pointer: non-zero marker
        (1 << DEPTH) - 1,           # PlanePick
        0,                          # PlaneOnOff
        0,                          # NextImage
    )


def gadget():
    return struct.pack(
        ">IhhhhHHHIIIIIhI",
        0,                    # ga_Next
        0, 0,                 # ga_LeftEdge, ga_TopEdge
        WIDTH, HEIGHT,        # ga_Width, ga_Height
        GFLG_GADGIMAGE,
        GACT_RELVERIFY,
        GTYP_BOOLGADGET,
        1,                    # ga_GadgetRender: non-zero marker
        1,                    # ga_SelectRender: non-zero marker
        0,                    # ga_GadgetText
        0,                    # ga_MutualExclude
        0,                    # ga_SpecialInfo
        0,                    # ga_GadgetID
        0,                    # ga_UserData
    )


def disk_object(icon_type, stack, drawer):
    body = struct.pack(">HH", 0xE310, 1)
    body += gadget()
    body += struct.pack(">BB", icon_type, 0)
    body += struct.pack(">I", 0)   # do_DefaultTool
    body += struct.pack(">I", 0)   # do_ToolTypes
    # NO_ICON_POSITION lets Workbench place the icon itself.
    body += struct.pack(">II", 0x80000000, 0x80000000)
    body += struct.pack(">I", 1 if drawer else 0)   # do_DrawerData
    body += struct.pack(">I", 0)   # do_ToolWindow
    body += struct.pack(">I", stack)
    assert len(body) == 78, len(body)
    return body


def build(icon_type=WB_TOOL, stack=8192):
    """No drawer icon is generated here on purpose: the installer creates
    the ZZ9000 drawer with (infos), which takes the system default drawer
    icon. That matches whatever icon set the user runs - a custom 4-colour
    drawer would look out of place next to MagicWB or NewIcons."""
    out = disk_object(icon_type, stack, drawer=False)
    out += image_struct() + to_planes(draw(selected=False))
    out += image_struct() + to_planes(draw(selected=True))
    return out


def preview():
    """ASCII rendering, so the artwork can be reviewed without an Amiga."""
    glyphs = {0: ".", 1: "#", 2: "O", 3: "+"}
    for state in (False, True):
        print("selected" if state else "normal")
        for row in draw(state):
            print("  " + "".join(glyphs[c] for c in row))
        print()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if "--preview" in sys.argv:
        preview()
        return 0

    check = "--check" in sys.argv
    status = 0
    work = [(rel, build()) for rel in TARGETS]
    for rel, data in work:
        path = os.path.join(root, rel)
        if check:
            existing = open(path, "rb").read() if os.path.exists(path) else None
            if existing != data:
                print("STALE %s (re-run %s)" % (rel, os.path.basename(__file__)))
                status = 1
            else:
                print("ok    %s" % rel)
        else:
            with open(path, "wb") as handle:
                handle.write(data)
            print("wrote %s (%d bytes)" % (rel, len(data)))
    return status


if __name__ == "__main__":
    sys.exit(main())
