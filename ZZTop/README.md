<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZTop — the ZZ9000 control panel

ZZTop is the Workbench program for everything you set on the ZZ9000: it
shows how the card is doing, chooses how native Amiga video appears on
the HDMI output, lines the picture up, runs the ZZ9000AX audio mixer,
edits the card's settings file on the microSD card without removing it,
and installs or restores firmware.

It installs to `SYS:Utilities/ZZ9000/ZZTop`, next to ZZPlay.

## Requirements

ZZTop 3 is built with the ReAction classes. They come with AmigaOS 3.2
(and 3.5/3.9); on AmigaOS 3.1 install ClassAct. If a class is missing,
ZZTop says which one and quits.

The window needs a screen of at least 640x480 pixels with an 8-point
font (any RTG Workbench, or PAL interlaced 640x512).

Settings stored on the card need firmware 2.3 or newer; live picture
calibration needs firmware 2.8 with a matching bitstream; the audio
mixer needs a ZZ9000AX daughterboard, `zz9k.library` and matching
firmware. Anything the installed stack cannot do is shown disabled,
with a line on the page saying why.

## The window

One window holds everything. Choose a page in the list on the left;
the page opens on the right with a short description under its title.
The window can be resized.

At the bottom are the status line and two buttons:

- **Save** writes every pending change: audio settings first, then the
  settings file on the SD card (`ZZ9000.CFG`).
- **Revert** throws away every unsaved change. Live changes (scanlines,
  picture alignment, sampling phase, audio) are put back exactly as they
  were when ZZTop opened or last saved, and the settings file is read
  again.

The window title says when something is unsaved and whether a
power-cycle will be needed. Quitting with unsaved changes asks whether
to **Save**, **Keep until reboot** (leave live changes running without
saving them), **Discard** or **Cancel**.

Settings take effect in one of two ways, and every page says which:

- **Live** — applied to the running card immediately (scanlines,
  picture alignment, sampling phase, capture window, audio). Save makes
  them survive a power-cycle.
- **At the next power-cycle** — the firmware reads them when the Amiga
  is switched on (output mode, interrupt, network address, SD boot, RTG
  options, Fast RAM). After saving, the status line says how many such
  settings are waiting.

**Settings → Advanced controls** adds three pages for detailed work:
**Capture** (under Display), **Scenes** (under Audio) and
**Diagnostics**. ZZTop remembers the choice.

## Status

What the card is and how it is doing: Zorro II or III, firmware
version, the features the installed firmware offers, FPGA temperature
(with the range seen since ZZTop started) and voltages, whether a
ZZ9000AX is fitted, the native video signal, the SD boot image and the
settings file on the card. It updates twice a second.

## Display

How the native Amiga picture is shown on the HDMI output.

**Output** and **Refresh** belong together: only the combinations the
installed firmware and bitstream support are offered, and changing the
output keeps the refresh when that pair exists, otherwise a valid one is
chosen and the status line says so. A short description explains the
selected pair. These take effect at the next power-cycle.

| Output | Refresh | Saved profile |
|---|---|---|
| 1280x1024 - full detail | 60Hz | `full_60` |
| 1280x1024 - full detail | Match Amiga | `full_exact` |
| 1920x1080 - centered | 60Hz | `centered_1080p_60` |
| 1920x1080 - centered | 50Hz | `centered_1080p_50` |
| 1920x1080 - centered | Match Amiga (experimental) | `centered_1080p_match` |
| 800x600 / 720x480 - filtered | 60Hz | `filtered_60` |
| 720x576 / 720x480 - filtered | 50Hz PAL, 60Hz NTSC | `filtered_pal` |
| 720x576 / 720x480 - filtered | PAL Amiga clock | `filtered_pal_exact` |
| 720x576 / 720x480 - filtered | NTSC Amiga clock | `filtered_ntsc_exact` |

The centered rows need a matched v2.8 stack (firmware capability bit 3
for centered 60 Hz, bits 3+4 for 50 Hz, bits 3-5 for
`centered_1080p_match`). `Match Amiga` on the full-detail output uses
fixed approximations (about 49.93/59.95 Hz); only `centered_1080p_match`
follows the Amiga's own raster. A stored centered profile the card
cannot run is saved as `full_60`.

When the old `ZZ9000-VCAP-800x600` or `ZZ9000-NS-VSYNC` environment
variables are set they override this choice; the page names them and
offers to adopt or remove them.

**Scanlines** (Off, Classic, Soft, Gradient) and which lines are
darkened apply live.

**Picture** shows whether framing and sampling are automatic or custom
and offers:

- **Align picture...** opens a native PAL or NTSC Hires screen with
  edge, safe-area and centre guides. The arrow keys move the picture one
  step (Shift: 16 steps); Enter keeps the new position, Escape puts back
  exactly what was there before. The screen stays open until the card
  confirms the restore; pressing Escape again closes it anyway, and
  ZZTop then reports the live state as unknown so Revert or a cold boot
  can recover it.
- **Find best phase...** (C28 capture engine) measures every sampling
  phase on a test pattern, picks the middle of the clean range and
  retests it. Keep the pattern screen in front while it runs. Enter
  keeps the result, Escape or Ctrl-C restores the previous phase.
- **Reset to automatic** returns framing, sampling phase and capture
  window to automatic, live.

Live framing needs matched firmware 2.8+ with a protocol-1 live-control
bitstream. A custom crop belongs to the capture path it was made on:
after switching between full-detail and filtered output, framing goes
back to automatic and the page asks you to align again after the
power-cycle.

## Capture (advanced)

The numbers behind the Picture controls, for fine work:

- **Sample mode**: Average (recommended) or the even/odd field only, for
  diagnosis. Takes effect at the next power-cycle.
- **Sampling phase** in the units of the detected capture engine (C28:
  -896..895, E7M: -255..255), with fine and coarse step buttons. Applied
  live and confirmed by the card before it can be saved.
- **Last phase sweep**: the result of the last Find best phase run, one
  character per four measured phases (`=` all clean, `+` mixed, `.` all
  dirty; `|` marks the current phase). Nothing is shown until a sweep
  has run.
- **Crop H / V** and **Capture width / height**, each with an
  **Automatic** box. Width is 256-1280 in steps of 16, height 100-1024.
  Values apply live when the firmware supports it and show both what was
  requested and what the card applied.

Save refuses a value the card has not confirmed, and says which.

## Audio

The ZZ9000AX mixer, live. Needs the daughterboard, `zz9k.library` and
firmware with the audio control plane; otherwise the page says which is
missing.

- **Scene** picks one of the eight scenes (filter, EQ, volume presets).
- **Paula level** and **AX level** set the mix. Their ranges come from
  the card's measured calibration, so the sum cannot exceed what the
  output limiter allows; the gauge shows the combined level against
  that limit. **Balance** sets the recommended parity mix (Paula at
  three quarters of its ceiling, AX at twice Paula).
- **Meters** show output and input peaks in dBFS, clipping, underruns
  and overruns since the page opened, and when the limiter had to step
  in.

The firmware saves audio settings itself; ZZTop's Save starts that and
waits for the card to confirm.

## Scenes (advanced)

Edits the active scene: its name, output lowpass filter (Hz), volume,
pre-gain, pan and the ten-band equaliser (50 = flat). The lowpass is
applied when you let go of the slider; the others follow the slider.

**Card calibration** holds the measured clean ceilings for Paula and
AX. They are specific to each card; changing them moves the mix limit.
Controls pause until the card confirms the new calibration.

## System

Card settings read at the next power-cycle.

- **Interrupt**: INT6 (default) or INT2 for every ZZ9000 driver (audio
  and network). Try INT2 when another card monopolises INT6 and the
  ZZ9000's devices misbehave. Set the card's J4 jumper to match: a
  mismatch locks the machine up when the network cable is connected.
- **Network address**: the card's built-in MAC, or your own
  (`00:11:22:33:44:55`), e.g. for a router reservation or after a board
  swap.
- **SD card boot**: boot Workbench from an HDF image in the root of the
  microSD card. Without a setting the firmware boots `zz9000.hdf` when
  it exists; switching this off stops SD boot even then.
- **Graphics**: Picasso96 off-screen bitmaps in card memory and the
  video overlay window. Both are on by default; turn them off only to
  work around a misbehaving application. ZZPlay's in-window video needs
  the overlay.
- **Memory**: Zorro III Fast RAM, where the firmware supports it. A
  change applies at the next reboot (a warm reset is enough, unlike the
  other settings here). The page shows what actually happened at boot,
  which can differ from the setting: the firmware withholds Fast RAM when
  the settings file was damaged, truncated, unreadable or too slow to
  read.

If `ENV:ZZ9K_INT2` or `ENV:ZZ9K_MAC` exist, they override the card file
for the drivers. The page shows them and offers to adopt their value
into the card file or remove them.

## Firmware

- **Install new firmware**: choose a firmware file, confirm, and ZZTop
  copies it to the card as `BOOT.bin`, keeping the previous image as
  `BOOT.bak`. Power-cycle to start it.
- **Restore previous firmware** puts `BOOT.bak` back as `BOOT.bin`. It
  talks to the running firmware, so it rescues an update that boots but
  misbehaves; a card that no longer boots needs the microSD card
  restored on another computer.

Both need firmware with the file-update protocol; the page says when the
installed firmware lacks it.

## Diagnostics (advanced)

Raw register values, every firmware capability bit in words, the board
address, a read-only Zorro bus check, a reset for the temperature range,
and the audio control-plane debug log (`T:zztop-audio.log`).

## How Save works

Save reads `ZZ9000.CFG` from the card again right before writing it and
changes only the settings you edited, so values written meanwhile by
someone else (the firmware's audio save, an edit on another computer)
are kept, and settings you never touched stay at the firmware default.
The firmware keeps the previous file as `ZZ9000.BAK`. Comments in a
hand-written file are not kept. If the new file would not fit the
firmware's size limit, nothing is written.

The firmware's audio save rewrites the whole file from what the card
read at power-on, which used to undo settings saved earlier in the same
session. ZZTop now puts those settings back right after an audio save.

Precedence is `ENV:` variable first, then the card file, then the
built-in default.
