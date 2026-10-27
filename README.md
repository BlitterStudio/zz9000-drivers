[![CI](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml)

# ZZ9000 Drivers for AmigaOS

This package installs the AmigaOS drivers, utilities, configuration panels, and
SDK runtime payloads for an MNT ZZ9000 Zorro II or Zorro III card. It is the
Amiga-side half of the stack; the matching FPGA logic and ARM firmware are in
[zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware).

Use firmware, bitstream, SDK payloads, and drivers from the same release. That
is especially important on Zorro II, where the stack negotiates the board's
shared-memory layout before SDK services are exposed.

## Components

| Component | What it provides |
|---|---|
| [Installer](installer/README.md) | The Commodore Installer drawer, release layout, and end-user installation flow. |
| [Picasso96 graphics](#picasso96-graphics) | `ZZ9000.card`, monitor settings, acceleration, DPMS, P96 video overlay, and custom modelines. |
| [Networking](#networking-and-diagnostics) | `ZZ9000Net.device`, the optional Roadshow `ZZ9000Net` template, `ZZNetReady`, and `ZZNetStats`. |
| [Board and command-line tools](docs/cli-tools.md) | `ZZDiag`, `ZZFwUpdate`, `ZZScanlines`, and their command-line reference. |
| [ZZCapture](ZZCapture/README.md) | Native-video timing observation and experimental A4000/C28 capture diagnostics. |
| [ZZ9000AX audio](ahi/README.md) | `zz9000ax.audio`, its AudioMode, and `mhizz9000.library` for PCM playback, recording, and hardware MP3 decoding. |
| [Poseidon USB](usb-poseidon/README.md) | `zzusbhw.device` for the external Poseidon 4.5 USB stack. |
| [SD-card boot](sd-boot/README.md) | `zzsd.device` in `BOOT.bin`, which boots an HDF file from the FAT-formatted microSD card. |
| [ZZTop](ZZTop/README.md) | The `ZZTop` control panel. |
| [SDK runtime payloads](sdk/README.md) | `zz9k.library`, `mpega.library`, `zz9k-picture.datatype`, `zz9k-sound.datatype`, SDK tools, and ZZPlay. |
| [AmiSSL provider](amissl/README.md) | The CPU-specific `amissl_v362.library` builds with supported ZZ9000 TLS crypto offload. |

## Install from a release

1. Download `zz9000-drivers-<tag>.lha` from
   [Releases](https://github.com/BlitterStudio/zz9000-drivers/releases).
2. Unpack it on the Amiga.
3. Open `ZZ9000Installer/Install ZZ9000`.
4. Reboot after installing or replacing drivers.

The installer places the drivers, tools, icons, Picasso96 settings, and optional
Roadshow template. It also stages the SDK runtime and selects the matching
AmiSSL build for the detected CPU when an AmiSSL 5.27 installation is present.
Its AmiGuide manuals install beside ZZTop and ZZPlay or into the documentation
drawer selected during installation.

ZZTop and ZZPlay install in `SYS:Utilities/ZZ9000/`. Expert installation can
add that drawer to the command path by updating its `;BEGIN ZZ9000` block in
`S:User-Startup`. Upgrades can remove the former `SYS:Tools/ZZTop` location.

`ZZTop` runs on AmigaOS 3.0 or newer. It fits screens down to NTSC HighRes
640x200 with an 8-pixel screen font, and PAL HighRes 640x256 with fonts up to
13 pixels. See the [ZZTop manual](ZZTop/README.md) for setup, configuration,
and feature requirements.

## 2.9.0 release

`ZZ9000.card` 2.14 changes the Zorro III boundary between Picasso96-owned
VRAM and firmware-owned memory. This is a matched update: firmware 2.9.0 and
its matching bitstream are required. Install the firmware, bitstream,
`ZZ9000.card`, and SDK payloads from the 2.9.0 release together; do not pair
these drivers with an older firmware release.

## Picasso96 graphics

Install Picasso96 before running the installer. It configures
`Devs:Monitors/ZZ9000` with `BOARDTYPE=ZZ9000`, `FAKENATIVEMODES=No`, and
`DISPLAYCHAIN=Yes`. Keep `DISPLAYCHAIN` enabled so the ZZ9000 can switch the
native Amiga signal through its flicker fixer.

Set `MASKEDBLITS=On` and `PLANAR2CHUNKY=On` in the P96 Preferences Editor for
hardware-accelerated masked blits and planar conversion. Keep `BLITTER` on and
`SOFTSPRITE` off. `BIGSPRITE` is optional for a larger pointer; `OVERCLOCK`
has no effect, and `BORDERBLANK` affects appearance only. `SYSTEM2SCREENBLITS`
can help on Zorro III; leave it at its default on Zorro II. A program that
requires a planar RTG screen needs `EmulatePlanar=On`.

Firmware that advertises `ZZ_FW_CAP_CUSTOM_MODE` accepts progressive custom
P96 timings. Width must be 320–2560 pixels and a multiple of eight; height must
be at least 200. Both axes need positive porch and sync values, totals no
larger than 4095, and a 25–165 MHz pixel clock that the PLL can produce within
0.5%. Use equal sync polarity and avoid doubled-clock or doubled-scan flags.
Test a mode in P96Prefs/Picasso96Mode before saving it. A rejected mode keeps
the last accepted `BoardInfo.ModeInfo` and blocks panning until another mode is
accepted.
For an initial 960x720 test, use 53.000 MHz; horizontal total 1184, front
porch 48, and sync width 96; vertical total 746, front porch 3, and sync width
4; with both syncs negative. This produces about 60.00471 Hz. Fixed packaged
timings retain the fixed-preset path, including its polarity handling. `QUIET`
is unavailable; `BELOW`, `LEFT-OF`, `RIGHT-OF`, `MONITORWIDTH`, and
`MONITORHEIGHT` place a second graphics board and do not affect a single
ZZ9000.

## Scandoubler output and refresh

ZZTop saves the chosen native-video profile to `ZZ9000.CFG`; it takes effect on
the next power cycle. It offers only profile and refresh pairs that the
installed firmware and bitstream advertise.

| Output | Refresh | `videocap_profile` |
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

The centered 60 Hz profile requires firmware capability bit 3; centered 50 Hz
requires bits 3 and 4; `centered_1080p_match` requires bits 3–5 and a matched
v2.8 stack. `full_exact` selects fixed PAL/NTSC approximations rather than
source-synchronised output. The [ZZTop manual](ZZTop/README.md) describes
picture alignment, sampling controls, and the capability checks for each mode.
`centered_1080p_60` places unchanged 1280x1024 content at `(320,28)` in a
1920x1080 raster. Its content rectangle is `[320,1600) x [28,1052)`; the
surrounding pixels are black. It uses a 148.5714286 MHz clock and 2200x1125
totals for approximately 60.02886 Hz. The Zorro III P96 profile separately
offers 1920x1080x32.

The firmware reads `ZZ9000.CFG` from the microSD-card root at boot. See
[firmware configuration](https://github.com/BlitterStudio/zz9000-firmware/blob/master/docs/configuration.md)
for every key and parser rule. `ENV:` settings and RTG ToolTypes take
precedence over the card file. Remove `ZZ9K_INT2`, `ZZ9K_MAC`,
`ZZ9000-VCAP-800x600`, `ZZ9000-NS-VSYNC[-NTSC]`, `ZZ9000-NO-OFFSCREEN`, and
`ZZ9000-NO-PIP` when moving those settings to the card file.

## Networking and diagnostics

`ZZ9000Net.device` is a SANA-II Ethernet driver and works with SANA-II-capable
stacks such as Roadshow, Miami DX, and AmiTCP. It programs the firmware
multicast hash filter for joined groups. `ZZNetReady` reports whether the card
is present and Ethernet has negotiated; `ZZNetStats` reports SANA-II counters
and firmware receive-backlog and drop registers. `ZZDiag` reports board,
firmware, VideoCap, USB, SD, audio, and Ethernet diagnostics.

`ZZFwUpdate` installs a `BOOT.bin` and, when it has a sibling `ZZ9K/` drawer,
the release's optional `*.ZZM` modules. It sends modules first and `BOOT.bin`
last, preserves the card's own `ZZ9000.CFG`, and keeps `BOOT.bak` rollback
images. It requires firmware with the FWUP protocol. Use the
[command-line tool manual](docs/cli-tools.md) for its backup and restore
procedure. ZZTop's Firmware page uses the same protocol and can also check
GitHub for a newer firmware release and install the complete bundle.

## Compatibility and status

| Area | Requirement or status |
|---|---|
| Base platform | `m68k-amigaos`, an MNT ZZ9000 Zorro II or Zorro III card, and Picasso96 for RTG graphics. |
| Audio | A ZZ9000AX daughterboard for AHI, MHI, and AX tools. |
| SD boot | `zzsd.device` is embedded in `BOOT.bin`; its 7424-byte boot-ROM limit is checked by CI. |
| Configuration | Firmware 2.3 or newer reads `ZZ9000.CFG`. |
| Display power management | Picasso96 DPMS needs firmware 2.7 or newer with the matching rebuilt bitstream. |
| USB | Interrupt and ISO paths require the matched firmware 2.9 and `zzusbhw.device` 2.2 pair. The [USB manual](usb-poseidon/README.md) and [qualification matrix](docs/usb-qualification-matrix.md) record tested devices and pending qualification. |
| RTG 2.9.0 | `ZZ9000.card` 2.14 requires matching firmware 2.9.0 and its matching bitstream because it changes the Zorro III P96/firmware memory boundary. |
| Zorro II SDK services | Generation 1 supplies a 64 KiB host heap; generation 2 supplies a 16 KiB host heap and reserves 48 KiB for the audio direct ring. The [SDK manual](sdk/README.md) lists supported payloads, PIP limits, fallbacks, and qualification status. |
| ZZCapture calibration | `phase`, `check`, `startup`, and `calibrate` remain experimental A4000/C28 diagnostics. Follow the [ZZCapture manual](ZZCapture/README.md) before running them. |

## Building and releases

GitHub Actions builds every component in
`sacredbanana/amiga-compiler:m68k-amigaos`, assembles tag releases, and checks
release contents. Run `make build-all` for the local equivalent. The
[build script](tools/build-all.sh), [CI workflow](.github/workflows/ci.yml),
and [release guide](RELEASING.md) define the artifact and tag procedures.

`make package-local` creates `zz9000-drivers-local.lha` and needs an `lha`
with compression support. CI uses the `lha` in the compiler image. Release
binaries come from CI; [binary policy](docs/BINARY_POLICY.md) describes the
source-tree exceptions.

## Provenance and license

This independent BlitterStudio-maintained fork is maintained by Dimitris
Panokostas and has no affiliation with MNT Research GmbH. MNT Research designs
and manufactures the ZZ9000 hardware. Send hardware questions to MNT Research
and driver, installer, or fork issues to this repository's
[issue tracker](https://github.com/BlitterStudio/zz9000-drivers/issues).

The original source is at <https://source.mnt.re/amiga/zz9000-drivers>.
Dimitris Panokostas contributed RTG optimisation, scanline tools, ZZTop
settings work, USB, SD boot, firmware update and restore, network diagnostics,
SDK and AmiSSL packaging, CI, releases, and installer updates. Xanxi created
the V1 and V2 scanline bitstreams. Henryk Richter wrote the network-driver
base in 2018.

SPDX-License-Identifier: `GPL-3.0-or-later`. See
<https://spdx.org/licenses/GPL-3.0-or-later.html>.
