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
the next cold boot. Nothing you change in ZZTop affects the running video
path until the card is power-cycled, except where a window says otherwise
(scanline settings and audio levels apply live).

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

**Capture…** opens sampling, framing and calibration controls. Framing
defaults to **Automatic** (`280/40` on full-rate paths, `188/26` on
filtered and Denise-adapter paths); **Custom** values are literal
per-machine overrides.

**Calibrate…** moves the picture with the cursor keys on a native PAL or
NTSC Hires screen, so what you align is the physical capture path:

- Arrow keys move the picture in the named direction, one crop unit at a
  time; hold Shift for 16-unit steps.
- **Enter** accepts the displayed pair as Custom values. It does not
  write the SD card.
- **Escape** restores the exact state from calibration entry, including
  the per-axis Automatic flags.
- **Done** stages the accepted preview; **Save** is the only action that
  writes `ZZ9000.CFG`. A cold boot later reproduces the saved pair.
Calibrate requires matched firmware 2.8-or-newer and a live-calibration
bitstream. On an older or mixed install the Automatic and numeric Custom
controls stay available and **Calibrate** stays disabled. A Custom pair is
tied to the capture path it was measured on (sample mode plus full-width
state); if staged settings select a different path, Calibrate and Custom
Save stay unavailable until the path is restored. Cancelling a capture or
reloading the window restores the live state it owned; if a control
acknowledgement times out, ZZTop keeps the window open and says the state
is unknown — retry when native frames return, or cold-boot to recover the
saved state.

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
## Firmware

The Firmware page shows the installed firmware version and update-protocol
status. Firmware that reports its patch level shows three numbers, such as
`2.8.1`; older firmware shows `2.8`. Firmware with the module capability also
lists every optional module and whether it loaded, is missing, or was rejected
for its ABI, hash, or fixed slot.

### Updating online

1. Check that **Board** names the right firmware build. ZZTop selects the
   build the card is running when the firmware identifies it (2.8 or later);
   otherwise it selects the plain Zorro II or Zorro III build. The A500 and
   A500+ builds are interchangeable.
2. Select **Check online**. ZZTop reads the latest firmware release from
   GitHub and shows it under **Latest**.
3. Select **Download and install** and confirm. ZZTop downloads the release
   archive, extracts `BOOT.bin` and direct `ZZ9K/*.ZZM` entries to a bounded
   temporary drawer in `T:`, and verifies `BOOT.bin` plus each module header
   and image SHA-256. It uploads modules first and `BOOT.bin` last, then
   removes the temporary drawer. Escape or the close gadget cancels the
   download.
4. Firmware without `ZZ_FW_CAP_FWUP_MODULE_DIR` receives only `BOOT.bin`.
   Reboot, then run the install again to add the optional modules.
5. Power-cycle the Amiga to start the new firmware.

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
one. Firmware older than 2.8 cannot report its build, so ZZTop then asks
before installing anything other than the plain Zorro II or Zorro III build.
A build for the wrong machine may not start, and recovery then needs the
microSD card in another computer.

The download needs up to 4 MB free in `T:`. GitHub limits anonymous release
queries to 60 per hour from one address; ZZTop reports when the limit is hit.
After an online install ZZTop stores the release tag in
`ENVARC:ZZTopFirmware`, so firmware that cannot report its patch level still
compares exactly after the next boot. Installing a chosen file or restoring
the previous firmware deletes it.

### Installing a file

1. Select **Choose file...** and select a release `BOOT.bin`.
2. ZZTop finds valid modules in its sibling `ZZ9K/` drawer and lists the
   files that will be installed. Select **Install**, confirm the request, and
   wait for the complete transfer. Modules are installed first, then ZZTop
   promotes the image to `BOOT.bin` and saves the previous image as
   `BOOT.bak`. `ZZ9000.CFG` is never installed.
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
