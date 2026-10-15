<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZTop 3

ZZTop is the ZZ9000 Workbench control panel. It shows card health, changes
native-video output and scanlines, calibrates the captured Amiga picture,
controls the ZZ9000AX mixer, edits `ZZ9000.CFG` on the microSD card, and
installs or restores firmware.

ZZTop runs on AmigaOS 3.0 or later. It uses the screen's font and lays its
window out for it, with compact and minimal forms for small screens: NTSC
HighRes 640x200 with an 8-pixel font such as topaz 8, and PAL HighRes 640x256
with fonts up to 13 pixels. On a screen too short for its font, ZZTop says how
many lines are missing instead of opening. It requires a detected, configured
ZZ9000.
`asl.library` is needed only when choosing a firmware file. **Check online**
needs a running TCP/IP stack (`bsdsocket.library`) and AmiSSL 5.
`zz9k.library` is optional, although the audio controls need its advertised
audio capability.

## Starting ZZTop

Start `ZZTop` from Workbench. It has no command-line options or Workbench
ToolTypes.

Choose a page from the list at the left. The normal list contains Status,
Display, Audio, System, and Firmware. Enable **Settings > Advanced controls**
to add Capture and Scenes as indented child pages, and Diagnostics as an
additional page. This choice is kept in `ENV:ZZTopAdvanced`; it is a program
setting, not a ToolType.

The selected page has a title and a short description. The footer has a status
line, **Revert**, and **Save**. The menus provide **Project > About**,
**Project > Quit**, **Edit > Save**, **Edit > Revert**, and **Settings >
Advanced controls**, with the shortcuts `?`, `Q`, `S`, `R`, and `A`.

## Saving, reverting, and quitting

Changes have one of two effects.

| Effect | Settings |
|---|---|
| Live | Scanlines, live picture alignment and sampling, capture geometry where supported, and audio controls |
| After a power cycle | Native-output profile, sampling mode, interrupt, MAC address, SD-card boot image, RTG options, and Fast RAM |

**Save** first validates pending MAC, HDF, and capture values. It saves audio,
repairs any non-audio settings that the firmware audio save replaced, rereads
`ZZ9000.CFG`, merges only the changed non-audio fields, then writes the merged
file. The status line identifies settings that need a power cycle.

**Revert** restores the saved live capture, scanline, and audio state, then
reloads the settings file. It reports a restore failure instead of pretending
that the previous state is known.

When quitting with live or audio changes, select **Save**, **Keep until
reboot**, **Discard**, or **Cancel**. With card-file-only changes, the choices
are **Save**, **Discard**, and **Cancel**.

`ZZ9000.CFG` handling requires firmware 2.3 or later. ZZTop keeps fields it did
not change when it rewrites the file. Firmware keeps the previous card file as
`ZZ9000.BAK`; comments in a hand-written file are not retained. A write that
would exceed the firmware file limit is rejected.

## Status

Status is read-only and refreshes the health values twice per second. It shows:

- card model and Zorro II or Zorro III bus;
- firmware version and advertised features;
- FPGA temperature, its minimum and maximum since ZZTop started, `VCCINT`, and
  `VCCAUX`;
- ZZ9000AX/audio availability and native-video signal information;
- SD-card boot state and `ZZ9000.CFG` read status and size; and
- a notice for pending changes.

## Display

Display controls the native Amiga picture on HDMI. It offers only output and
refresh pairs supported by the installed firmware and bitstream. Changing an
output keeps the selected refresh when that pair exists; otherwise it selects
a supported refresh. The selected profile applies after a power cycle.

| Output | Refresh | `videocap_profile` |
|---|---|---|
| 1280x1024 - full detail | 60 Hz | `full_60` |
| 1280x1024 - full detail | Match Amiga | `full_exact` |
| 1920x1080 - centered | 60 Hz | `centered_1080p_60` |
| 1920x1080 - centered | 50 Hz | `centered_1080p_50` |
| 1920x1080 - centered | Match Amiga | `centered_1080p_match` |
| 800x600 / 720x480 - filtered | 60 Hz | `filtered_60` |
| 720x576 / 720x480 - filtered | 50 Hz PAL, 60 Hz NTSC | `filtered_pal` |
| 720x576 / 720x480 - filtered | PAL Amiga clock | `filtered_pal_exact` |
| 720x576 / 720x480 - filtered | NTSC Amiga clock | `filtered_ntsc_exact` |

Centered profiles are capability-gated. An unavailable centered profile is
stored as `full_60`. `Match Amiga` with the full-detail output uses fixed
approximations; `centered_1080p_match` follows the Amiga raster when the
firmware advertises that capability.

`ENV:ZZ9000-VCAP-800x600` and `ENV:ZZ9000-NS-VSYNC` override this selection.
When either is present, the page identifies the override and provides **Use
it** to copy its meaning into pending card-file settings, or **Remove** to
delete its `ENV:` and `ENVARC:` copies.

The page also provides these live controls:

| Control | Action |
|---|---|
| **Scanlines** | Selects `Off`, `Classic`, `Soft`, or `Gradient`. |
| **Darken** | Selects `Odd lines` or `Even lines`. |
| **Picture** | Reports the current framing and sampling state. |
| **Align picture...** | Opens a native PAL or NTSC Hires calibration screen. Arrow keys move one step, Shift plus an arrow moves 16 steps, Enter keeps the position, and Escape restores the position from before the screen opened. |
| **Find phase...** | Measures a C28 capture-engine test pattern, selects the middle of a clean range, and retests it. Keep the pattern screen in front. Enter keeps the result; Escape or Ctrl-C restores the previous phase. |
| **Reset auto** | Returns framing, sampling phase, and capture geometry to automatic settings. |

Live picture work needs firmware 2.8 with a matching bitstream. Alignment also
needs stable native frames and a capture path that matches the staged output
profile and sampling mode. Capture geometry needs firmware acknowledgement.
After changing between full-detail and filtered capture paths, align the
picture again after the power cycle.

## Capture

Capture is an advanced child page under Display. It is disabled when firmware
configuration support is unavailable, and individual controls are disabled
when their live-capture capability is unavailable.

| Control | Values and behaviour |
|---|---|
| **Sample mode** | `Average (recommended)`, `Even fields (diagnostic)`, or `Odd fields (diagnostic)`. This applies after a power cycle. |
| **Sampling phase** | Fine and coarse controls for the active capture engine. The step buttons are `-28`, `-1`, `+1`, and `+28`. C28 accepts `-896..895`; E7M accepts `-255..255`. ZZTop saves a live value only after the card confirms it. |
| **Last phase sweep** | Result of the most recent phase search. `=` means every measured phase in the group was clean, `+` means mixed, `.` means dirty, and `|` marks the current phase. |
| **Crop H** and **Crop V** | Signed crop values with an **Automatic** control. |
| **Capture width** and **Capture height** | Width is `256..1280` in 16-pixel steps; height is `100..1024`. Each has an **Automatic** control. |
| **Picture actions** | **Align picture...**, **Find phase...**, and **Reset auto** are also available here. |

Typed values are committed before changing page and before Save, Revert, or a
quit decision. An Automatic crop, phase, width, or height clears its native
configuration key. Geometry requests show both the requested value and the
value acknowledged by the card.

## Audio

Audio controls the ZZ9000AX mixer live. It requires the daughterboard,
`zz9k.library`, the firmware audio-control capability, and a responding
control-state request. The page identifies the missing condition. **Retry** is
available only for a transient audio-control-plane response failure.

- **Scene** selects one of eight scenes.
- **Paula level** and **AX level** set the two mix levels. Their `0..255`
  ranges are constrained by the card calibration and current mix boundary.
- **Balance** applies the qualified mix preset: Paula at three quarters of its
  ceiling and AX at twice the Paula value.
- **Mix limit** shows the weighted combined level against the limit.
- **Meters** show output left/right and input left/right peaks in `-60..0`
  dBFS, plus output clipping, underruns, limiter activity, input clipping, and
  overruns since the page opened.

Save asks the firmware to persist the audio state. Meter values need the
separate audio-metering capability.

## Scenes

Scenes is an advanced child page under Audio. It edits the active scene.

| Group | Controls |
|---|---|
| Scene | **Scene** selector and **Name**, up to 16 characters |
| Master | **Output lowpass** (`1..23900` Hz), **Volume**, **Pre-gain**, and **Pan** (`0..100`, where `50` is centre) |
| Equaliser (Hz) | Ten bands: `31`, `63`, `125`, `250`, `500`, `1k`, `2k`, `4k`, `8k`, and `16k`; the middle position is flat |
| Card calibration | **Paula clean ceiling** and **AX clean ceiling**, each `1..4095` |

Volume, pre-gain, pan, and equaliser controls update while being dragged.
Output lowpass is applied after release. Changing either calibration value
blocks other audio edits until the card acknowledges the paired calibration.

## System

System settings apply after a power cycle and are disabled when firmware
configuration support is unavailable.

| Group | Controls and settings |
|---|---|
| Interrupt | **INT6 (default)** or **INT2**. Use INT2 only when an INT6 conflict prevents ZZ9000 devices from working. Set J4 to the selected interrupt; a mismatch can lock the machine when a network cable is connected. |
| Network | **Use the card's built-in address** or enter a MAC address as `aa:bb:cc:dd:ee:ff` (hyphens are also accepted). |
| SD card boot | **Boot from an SD card image** and an image filename in the microSD root. An empty setting leaves the firmware default, `zz9000.hdf`; disabled boot writes `hdf = off`. |
| Graphics | **Off-screen bitmaps in card memory (for incompatible software)** and **Video overlay window (needed by in-window ZZPlay)**. Both default to on. |
| Memory | **Zorro III Fast RAM**, with an effective-state message from the boot result. |

`ENV:ZZ9K_INT2` and a valid `ENV:ZZ9K_MAC` override their card-file values.
For either, select **Use it** to adopt the value or **Remove** to delete both
its `ENV:` and `ENVARC:` copies. The precedence is `ENV:` override, card file,
then firmware default.

Fast RAM is fail-closed. The firmware leaves it disabled after a malformed,
truncated, missing, unreadable, slow, or failed configuration read. A saved
Fast RAM change takes effect at the next cold boot.

## Firmware

The Firmware page shows the installed firmware version and update-protocol
status. Firmware that reports its patch level shows three numbers, such as
`2.8.1`; older firmware shows `2.8`.

### Updating online

1. Check that **Board** names the right firmware build. ZZTop selects the
   build the card is running when the firmware identifies it (2.8 or later);
   otherwise it selects the plain Zorro II or Zorro III build. The A500 and
   A500+ builds are interchangeable.
2. Select **Check online**. ZZTop reads the latest firmware release from
   GitHub and shows it under **Latest**.
3. Select **Download and install** and confirm. ZZTop downloads the release
   archive for the selected board, extracts `BOOT.bin` to `T:`, checks its
   CRC-32, size and Zynq boot header, then installs it like a chosen file.
   Escape or the close gadget cancels the download. The temporary image is
   deleted after a successful install and kept, selected for **Install**,
   after a failed one.
4. Power-cycle the Amiga to start the new firmware.

**Latest** shows the release tag and one of these results:

| Result | Meaning |
|---|---|
| `update available` | The release is newer than the installed firmware. |
| `installed` | The installed firmware is the latest release. |
| `may be newer` | The card cannot report its patch level and the release is a patch release of the same `major.minor`. |
| `older than installed` | The card runs a newer development build. |

Pre-releases are never offered. The archive's `ZZ9000.CFG` is not installed;
the card keeps its own settings file. ZZTop refuses an image for the other
Zorro bus and asks again before installing a build other than the running
one. A build for the wrong machine may not start, and recovery then needs the
microSD card in another computer.

The download needs up to 4 MB free in `T:`. GitHub limits anonymous release
queries to 60 per hour from one address; ZZTop reports when the limit is hit.
After an online install ZZTop stores the release tag in
`ENVARC:ZZTopFirmware`, so firmware that cannot report its patch level still
compares exactly after the next boot. Installing a chosen file or restoring
the previous firmware deletes it.

### Installing a file

1. Select **Choose file...** and select a `bin`, `rom`, or `img` file.
2. Select **Install**, confirm the request, and wait for the transfer to
   finish. ZZTop promotes the selected file to `BOOT.bin` and saves the
   previous image as `BOOT.bak`.
3. Power-cycle the Amiga to start the new firmware.

### Restoring the previous firmware

**Restore previous firmware** promotes `BOOT.bak` to `BOOT.bin`. This discards
the current `BOOT.bin` and leaves no backup. It works only while the card can
boot and communicate with ZZTop; restore an unbootable card's microSD card on
another computer. Transfers and restore are disabled until the updater probe
succeeds, and during a transfer.

## Diagnostics

Diagnostics is an advanced page for observation and safe checks.

- **Board** shows the Zorro revision, board address, FPGA temperature, and its
  session range.
- **Raw registers** lists hardware and firmware versions, capability bits,
  temperature, `VCCAUX`, `VCCINT`, USB, SD-card and SD-boot states, scanline
  mode and parity, video-capture statistics, audio configuration, and updater
  status.
- **Capabilities** lists video profile, live adjustment, 1080p, 50 Hz, sync,
  scanout-origin, geometry, geometry acknowledgement, and statistics support,
  plus SDK audio-control and metering support.
- **Run bus probe** reads the identity register eight times and reports the
  result.
- **Reset temperature range** clears the session temperature range.
- **Audio control-plane debug log (`T:zztop-audio.log`)** enables or disables
  the audio debug log.

Dynamic register and temperature values refresh twice per second.

## Settings file fields

ZZTop reads and writes the following current non-audio fields. Values marked
Automatic are omitted or commented out when saved.

| Field | Values |
|---|---|
| `videocap_profile` | `full_60`, `full_exact`, `filtered_60`, `filtered_pal`, `filtered_pal_exact`, `filtered_ntsc_exact`, `centered_1080p_60`, `centered_1080p_50`, `centered_1080p_match` |
| Legacy native-video fields | `videocap_mode = pal|800x600`, `videocap_shres = full|filter`, `nonstandard_vsync = off|pal|ntsc` |
| `videocap_sample` | `average`, `even`, `odd` |
| `videocap_crop_h`, `videocap_crop_v` | `0..4095`, or Automatic |
| `videocap_phase` | `-255..255`, or Automatic |
| `videocap_c28_phase` | `-896..895`, or Automatic |
| `videocap_width` | `256..1280`, in 16-pixel steps, or Automatic |
| `videocap_height` | `100..1024`, or Automatic |
| `scanline_mode` | `0..3` |
| `scanline_parity` | `0` or `1` |
| `int2`, `offscreen_bitmaps`, `video_overlay`, `fast_ram` | `on` or `off` |
| `mac` | Six hexadecimal octets separated by `:` or `-` |
| `hdf` | Image filename in the microSD root, or `off` |

The firmware audio save owns `audio_active`, `audio_baseline`,
`audio_ceiling_paula`, `audio_ceiling_ax`, and the eight scene groups
`audio_scene<N>_{lpf,eq01,eq23,eq45,eq67,eq89,out,pan,nm1..nm8}` for
zero-based `N=0..7`. ZZTop preserves these fields while completing a shared
Save operation.

## History and status

ZZTop 3 replaces the earlier ZZTop 2 windows with one window of pages, a
global Save and Revert, and a layout that fits screens down to 640x200.
