<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZTop — the ZZ9000 control panel

ZZTop is the Workbench program for the card's everyday controls: it shows
board and firmware status, edits `ZZ9000.CFG` on the microSD card without
pulling the card, adjusts the scandoubler and the audio mixer, and
installs or restores firmware. It needs no shell and no configuration
files of its own.

From v2.8 it installs to `SYS:Utilities/ZZ9000/ZZTop`, sharing one drawer
with ZZPlay. The installer offers to put that drawer on the command path
(`SYS:Utilities` itself is not searched recursively) and offers to delete
an old `SYS:Tools/ZZTop` when upgrading.

Most controls write the SD card's `ZZ9000.CFG`, which firmware reads at
the next cold boot. Scanlines, audio levels and explicitly live capture
calibration also affect the running card. Live changes still need Save to
survive a power cycle.

## Main window

The main window reads back the board identity, firmware version and
capabilities, and the current capture state. Two buttons open the
windows you will use most, **Scandoubler** and **Audio**; the same
windows are also on the **Settings** menu for screens where the buttons
do not fit.

## Settings — Scandoubler

The Scandoubler window owns native Amiga video: how the captured chipset
picture is presented on the HDMI output.

**Output** and **Refresh** are dependent selectors. Only combinations the
installed firmware and bitstream support are offered; changing the output
keeps the refresh when that pair exists, otherwise a valid refresh is
selected visibly.

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

The centered rows need a matched v2.8 stack: ZZTop checks the firmware
capability register (bit 3 for centered 60 Hz, bits 3+4 for 50 Hz, bits
3-5 for `centered_1080p_match`) and offers only what the card can do. A
stored centered profile the stack cannot support is replaced by `full_60`
at the next save, from any configuration window. `Match Amiga` on the
full-detail output selects fixed approximations (about 49.93/59.95 Hz);
only `centered_1080p_match` actually follows the Amiga's own raster.

Choose the pair, press **Save**, then power-cycle. Scanline mode and
parity apply immediately but still need **Save** to survive the power
cycle.

### Capture and live calibration

**Capture…** separates live hardware values, edits staged by **Done**, and
values persisted by the Scandoubler window's **Save**. Opening the window
or leaving a field untouched does not turn a live Automatic value into a
saved override.

- **Sampling Phase** and the ±1/±28 buttons apply live. The title names the
  C28 or legacy E7M engine; its signed range and step units are used
  throughout. C28 requires a qualified, locked C28 input clock. E7M uses
  its own ready/error/status layout, not the C28 clock gate.
- **Capture W / H** requests a window at the next stable native vblank.
  Width is 256–1280, aligned to 16 words; height is 100–1024 in the existing
  capture-window units. Clear either axis for Automatic independently.
  The status distinguishes the requested overrides from the resolved
  VDMA word width/source-row count. It reports applied only after the
  firmware acknowledges successful programming; RTG vblanks do not count.
  This needs the geometry-ACK firmware capability (bit 10 plus bit 8).
- Crop fields stage per-axis overrides. An empty field stages Automatic
  for that axis. **Align picture...** is the live framing editor. Automatic crop
  is resolved by the capture path and source class, not saved as a number.
- **Reset to Auto** resets the supported crop, phase and window controls live,
  then stages removal of their active-domain overrides with Done. Editing
  only phase afterwards leaves crop/window Automatic. A legacy geometry
  stack without readback/ACK disables Window and Reset to Auto rather than
  claiming that an unconfirmed reset succeeded. Cancel stops later
  steps of the reset: it does not force phase/window requests for components
  the cancelled reset never reached.

**Align picture...** opens a native PAL or NTSC Hires alignment screen:

- Arrows move the picture in the named direction, one crop unit at a
  time; Shift selects 16-unit steps.
- Enter accepts the displayed crop pair; Escape queues restoration of the
  exact entry state, including per-axis Automatic flags. Pending requests
  are reconciled without blocking keyboard input for the frame timeout.
- Accepted previews remain live. Done stages their values; Save alone
  writes `ZZ9000.CFG`. A cold boot reproduces the saved settings. Once
  Done accepts, later queued edits or Close do not alter that decision.

To populate **Last phase sweep**, open **Scandoubler -> Capture...**,
then select **Find phase...**. Keep the native pattern screen in front
while it sweeps and retests. Press **Enter** at the final prompt to accept
the selected phase; **Escape/Ctrl-C** restores the entry phase. Then use
Capture **Done** to stage it and Scandoubler **Save** to persist it.

**Find phase...** is C28-only and requires the frozen-field metadata
interface. It opens a native SuperHires 8-bit pattern with the verified
PAL/NTSC standard; a native caller's interlace mode is preserved, while an
RTG caller requests a progressive native test pattern. It verifies the
actual screen/mode, front-screen ownership, phase, source flags and frozen
geometry throughout acquisition. Each measured phase is applied once,
settling fields are discarded, and the selected circular eye is refined
and retested before acceptance. Escape/Ctrl-C restores the child's entry
phase before its screen closes. Progress text stays in the unsampled
header band, and the pointer is hidden so UI overlays cannot spoil the
measured pattern.

**Last phase sweep** is a measured pattern sweep, not a continuous live
eye: each glyph groups four coarse bins (`=` all clean, `+` mixed,
`.` all dirty); `|` marks the last confirmed phase group. Unsupported
E7M sampling and an unmeasured/invalidated sweep have no fabricated eye.

Capture **Cancel** restores the immutable parent entry phase, crop and
window even after a child sweep was accepted. Reload/closing Scandoubler
restores its unsaved live changes; a successful Save rebases that anchor.
Unconfirmed commands cannot be staged or saved. A failed restore leaves
the window open with live state explicitly unknown: retry when native
frames/clock return, or cold-boot to recover. Close/Cancel remains serviced
while normal phase/window/Automatic requests await acknowledgement.

Live framing requires matched firmware 2.8+ and a protocol-1 live-control
bitstream. Custom crop is tied to its sample/full-width capture path;
Align picture and Custom Save stay unavailable while staged settings select a
different path. Other numeric configuration edits can still be staged on
older stacks, but disabled live controls do not confirm running hardware.

## Settings — Audio

The Audio window is the operator surface of the firmware-owned audio
scene: master mix, per-device level trims, and live level metering. It
opens only when the running firmware advertises the audio control plane;
on older firmware or a machine without the ZZ9000AX daughterboard the
button is greyed out and playback is unaffected.

- **Baseline and ceilings.** The Paula and AX sliders are bounded by the
  card's own measured calibration, read live from the firmware. The
  **Balance** preset applies the firmware's parity formula (Paula at
  three quarters of the ceiling, AX at twice Paula, capped by the
  ceiling), which is also what an unconfigured card boots with.
- **Save** persists the scene — all eight scenes keep their names and
  calibration values — into `ZZ9000.CFG`.
- While the firmware confirms a new ceiling pair, edits and Save pause
  and the window retries once per second; if it stays pending, close and
  reopen Audio to read the live state. If the boundary read fails, reopen
the window once the control service responds; no settings changed.

Recording setup, jumper positions and the audio driver details have
their own manual (`ahi/README.md` in this repository), shipped as
`ahi.guide` in the installer drawer.

## Settings — Other Settings

The remaining card-level options. All of them take effect at the next
cold boot; native-video controls are not here, they live in Scandoubler.

**INT2** moves the card's interrupt from INT6 to INT2 for every driver at
once — audio (AHI and MHI) and networking. Some accelerators, SCSI
controllers and network cards monopolise INT6; if the card's devices
misbehave or do not appear, this is the switch to try. It writes
`int2 = on`, which is safer than the old `ENV:ZZ9K_INT2` variable because
all three drivers can no longer disagree about the line; an existing ENV
variable still wins, so remove it when you set this.

**MAC** overrides the card's Ethernet address. `ZZ9000Net.device` adopts
whatever the firmware reports, so a router reservation or a duplicate
address after a board swap is fixed here, once, instead of per stack.
Enter six colon-separated bytes (`00:11:22:33:44:55`); an absent or
invalid entry keeps the card's built-in address.

**HDF** names a root-level hard-disk image on the microSD card that the
firmware presents to the Amiga at boot — booting Workbench straight from
the SD card with no hard drive attached. Type the file name of an HDF
placed in the card's root; clearing the entry disables SD boot.

**Fast RAM** controls Zorro III Fast RAM where the running firmware supports
the fail-closed boot gate. The status line reports the *effective* boot result,
not only the saved choice: a configured-on value withheld for an invalid,
truncated, unreadable, or slow configuration is distinct from enabled. Older
firmware leaves the control disabled as unavailable. Every fast_ram change,
ZZTop Save or manual SD edit, applies at the next reboot (warm reset re-reads
the card).

**Offscreen bitmaps** (default on) is the switch for Picasso96 off-screen
bitmaps in card memory — the accelerated path that keeps intermediate
bitmaps next to the display. Turn it off only as an escape hatch when an
application misbehaves with card-side bitmaps.

**Video overlay** (default on) is the switch for the Picasso96 video
window, the hardware-scaled overlay that in-window video playback uses.
ZZPlay video requires it; with the overlay off, applications lose
in-window video (ZZPlay's dedicated fullscreen screen still plays).

## Project menu

- **Update Firmware…** copies a firmware image to the SD card as
  `BOOT.bin`, keeping the previous image as `BOOT.bak`. Power-cycle
  afterwards so the card boots it.
- **Restore Backup…** promotes `BOOT.bak` back to `BOOT.bin` after a
  confirmation. It talks to the running firmware, so it recovers an
  update that boots but misbehaves; a card that does not boot at all
  needs the microSD removed and restored on another computer.
- **Audio Debug Log** toggles the audio driver's debug output.
- **About** shows the component versions; **Quit** exits.

## Save rules

Each window saves only its own edits, preserving supported values from
the other sections. Absent native-video keys stay inactive during a
general Settings save; only Scandoubler Save activates the displayed
native settings. Generated files are written in compact formatting to
stay within the firmware's 4 KiB limit, so per-setting comments in a
hand-written file are not retained; a generation failure leaves the
existing file untouched.

Precedence is always `ENV:` variable first, then the config file, then
the built-in default. A lingering ENV variable hides the saved value, so
remove `ZZ9K_INT2`, `ZZ9K_MAC` and the legacy video variables when you
migrate to `ZZ9000.CFG`.
