[![CI](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml)

# ZZ9000 Drivers for AmigaOS

AmigaOS driver and utility package for the MNT ZZ9000 Zorro II/III card.
It includes RTG graphics, SANA-II networking, ZZ9000AX audio, Poseidon
USB, SD-card boot support, firmware-update tooling, diagnostics, and a
Commodore Installer drawer for end-user releases.

This repository contains the Amiga-side drivers and tools. Matching FPGA
logic and ARM firmware live in
[zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware).

## Fork Notice

This is an independent BlitterStudio-maintained fork of the original MNT
ZZ9000 AmigaOS driver sources. It is maintained by Dimitris Panokostas
and is not affiliated with, endorsed by, or supported by MNT Research
GmbH. The ZZ9000 hardware itself is designed and manufactured by MNT
Research GmbH.

Hardware questions belong with MNT Research. Driver, installer, and
fork-specific issues belong in this repository's
[issue tracker](https://github.com/BlitterStudio/zz9000-drivers/issues).

Upstream pre-fork source: <https://source.mnt.re/amiga/zz9000-drivers>

## What Changed Since the Original MNT Drivers

The original MNT drivers provided the essential AmigaOS connection to the
ZZ9000. This independent BlitterStudio fork keeps that foundation and builds a
complete end-user package around it. The most important differences are:

| Improvement | What an Amiga owner gets |
|---|---|
| **One guided installer** | A standard Commodore Installer puts the graphics, network, audio, USB and SD drivers in the right places, installs icons and tools, updates Picasso96 settings, and offers the correct extras for the detected CPU. Upgrades are handled without asking users to copy a collection of files by hand. |
| **ZZTop as the card's control centre** | ZZTop shows firmware and board status, edits `ZZ9000.CFG` directly on the card, changes scanlines and native-video settings, performs live picture calibration, and can install or restore firmware without removing the microSD card. |
| **Better native and RTG video** | The current stack offers clear native-output choices, full-detail 1280x1024 capture, the optional 1280x1024 image centered in a 1920x1080 signal, 1920x1080x32 RTG modes on Zorro III, improved acceleration, monitor sleep/DPMS, modern scanlines, and numerous correctness fixes. |
| **ZZPlay media playback** | The installed **ZZPlay** Workbench application uses the ZZ9000's ARM processors to decode MPEG-1 video and MP3/MP2 audio. Video can appear in a hardware-scaled Workbench window or on a dedicated screen. |
| **Hardware-assisted AmiSSL** | The installer can add a CPU-matched AmiSSL library with ZZ9000 acceleration built in. Supported TLS handshakes and encrypted data are offloaded automatically for existing AmiSSL applications; unsupported operations remain in software. |
| **A fuller set of working devices** | The package includes maintained SANA-II networking, ZZ9000AX AHI/MHI audio, Poseidon USB, SD-card boot support, firmware update/recovery, and focused board and network diagnostics. |
| **Useful acceleration on Zorro II too** | Matched 2 MB and 4 MB releases safely expose a small shared service area for images, archives, audio and AmiSSL. A 4 MB card can additionally provide one bounded ZZPlay picture-in-picture source; 2 MB cards deliberately do not advertise PIP. |
| **Repeatable releases** | Every component is built with pinned toolchains in CI, assembled into one installer drawer, and checked so stale local binaries cannot accidentally enter a release. |

The installer also brings in the matching SDK runtime: `zz9k.library`,
`mpega.library`, the optional accelerated picture DataType, command-line tools,
and the ZZPlay application. For best results, use drivers, firmware and SDK
payloads from the same release—particularly on Zorro II.

## Quick Start

For normal installation, use the latest GitHub Release zip:

1. Download `zz9000-drivers-<tag>.zip` from
   [Releases](https://github.com/BlitterStudio/zz9000-drivers/releases).
2. Unpack it on the Amiga.
3. Double-click `ZZ9000Installer/Install ZZ9000`.
4. Reboot after installing or replacing drivers.

The installer handles driver placement, tool installation, icons,
Picasso96 settings, the optional Roadshow NetInterface template, and
network setup prompts. Current releases also install `ZZTop`, which can
edit the SD-card `ZZ9000.CFG` file for MAC, INT2, native-video,
scanline, and boot-HDF settings. The installer also installs the SDK
runtime (`zz9k.library`, `mpega.library`, and the picture datatype) and
the per-CPU accelerated `amissl.library` (auto-detecting the CPU,
installing the matching build, and backing up the stock library once).

## Compatibility

- Target OS/toolchain: `m68k-amigaos`
- Hardware: MNT ZZ9000 Zorro II or Zorro III card
- Optional hardware: ZZ9000AX daughterboard for AHI/MHI/AX tools
- RTG stack: Picasso96
- Networking: any SANA-II capable stack, such as Roadshow, Miami DX, or
  AmiTCP
- USB: Poseidon, with a matching firmware USB mailbox implementation

Several features require current ZZ9000 firmware. In particular,
`ZZFwUpdate` needs firmware with FWUP protocol support, USB needs the
firmware USB stack, scanline V2 controls need the matching bitstream,
Picasso96 DPMS needs firmware 2.7+ with a matching rebuilt bitstream, and
`zzsd.device` is packed into `BOOT.bin` rather than installed as a normal
AmigaOS file.

SDK offload services are bounded, but no longer audio-only, on Zorro 2.
Current matched firmware, bitstream, SDK runtime, and drivers negotiate and
acknowledge the exact board layout before exposing one shared 64 KiB
CPU-visible host heap. `mpega.library`, `mhizz9000.library`, ZZPlay standalone
MP3, streamed `zz9k-view`, `zz9k-picture.datatype`, streamed archive work, and
the accelerated `amissl.library` use compact host-window/card-only layouts on
both shipped 2 MiB and 4 MiB profiles. The 4 MiB profile additionally provides
one 224 KiB PIP source for video frames that fit; the 2 MiB profile has no PIP.

Keep firmware, bitstream, SDK payloads, and drivers from the same release on
Zorro 2. Invalid or unacknowledged generation-1 descriptors deliberately fail
closed. Descriptor-absent legacy 4 MiB compatibility instead retains the
historical fixed 64 KiB host window only; legacy 2 MiB and unknown sizes reject
it. The negotiated 64 KiB heap is shared across clients, so concurrent image,
archive, audio, DataType, and TLS work can contend. Not every low-level SDK
diagnostic has been converted from the legacy default shared heap. See the SDK's
[Zorro II service matrix](https://github.com/BlitterStudio/zz9000-sdk/blob/master/docs/zz9k-zorro2-services.md)
for exact client limits and fallback behavior. An 8 MiB software profile
exists for future work, but no 8 MiB bitstream variant is shipped.

## Components

| Area | Artifact | Installed to | Notes |
|------|----------|--------------|-------|
| RTG graphics | `ZZ9000.card` | `Libs:Picasso96/` | Picasso96 RTG driver using the native `BT_MNT_ZZ9000` board identity, accelerated VRAM/PIP paths, and firmware-gated DPMS monitor power management. |
| P96 settings | `Picasso96Settings` / `Picasso96Settings-Z3` | `Devs:Picasso96Settings` | Installer backs up an existing file to `Devs:Picasso96Settings.pre-ZZ9000-2.4`, advertises Standby/Suspend/Active Off DPMS support, and offers a Zorro III high-memory profile with 1920x1080x32 enabled. |
| Networking | `ZZ9000Net.device` | `Devs:Networks/` | SANA-II Ethernet driver. |
| Network template | `ZZ9000Net` | `Devs:NetInterfaces/` | Optional Roadshow DHCP template installed by the installer. |
| Network diagnostics | `ZZNetStats` | `C:` | Dumps SANA-II counters plus firmware RX backlog/drop registers. |
| Board diagnostics | `ZZDiag` | `C:` | Dumps board identity, firmware, VideoCap, USB, SD, AX/audio, and Ethernet diagnostic registers. |
| AHI audio | `zz9000ax.audio` | `Devs:AHI/` | ZZ9000AX playback and stereo RCA recording driver. Runtime setup and tunables are documented in [ahi/README.md](ahi/README.md). |
| AHI mode | `ZZ9000AX` | `Devs:AudioModes/` | AudioMode file generated by the AHI build. |
| MHI audio | `mhizz9000.library` | `Libs:MHI/` | Exposes the AX hardware MP3 decoder to MHI-aware players. |
| USB | `zzusbhw.device` | `Devs:USBHardware/` | Poseidon USB hardware driver. See [usb-poseidon/README.md](usb-poseidon/README.md). |
| SD boot | `zzsd.device` | Firmware `BOOT.bin` | Size-constrained boot driver for FAT32-hosted HDF boot. See [sd-boot/README.md](sd-boot/README.md). |
| Configuration | `ZZTop` | `SYS:Utilities/ZZ9000/` | GUI for hardware readback, firmware update/restore, and the SD-card `ZZ9000.CFG` settings (Project > Settings; needs firmware 2.3+). See [the ZZ9000 drawer](#the-zz9000-drawer). |
| Scanlines | `ZZScanlines` | `C:` | CLI for scanline V1/V2 modes. |
| Firmware update | `ZZFwUpdate` | `C:` | Pushes `BOOT.bin` or another root-level file to the ZZ9000 FAT32 microSD card over Zorro. |
| SDK services | `zz9k.library` | `Libs:` | AmigaOS gateway to the SDK v2 firmware services (image/video decode, audio, compression, crypto). Built from the pinned [zz9000-sdk](https://github.com/BlitterStudio/zz9000-sdk) ref by `sdk/build.sh`. |
| MP3 decode | `mpega.library` | `Libs:` | ARM-accelerated drop-in MPEGA replacement (from zz9000-sdk). |
| Picture datatype | `zz9k-picture.datatype` | `SYS:Classes/DataTypes/` | Hardware-accelerated picture datatype; JPEG/PNG descriptors staged inactive in `SYS:Storage/DataTypes` (from zz9000-sdk). |
| SDK tools | `zz9k-info`, `zz9k-services`, `zz9k-view`, `zz9k-mp3`, `zz9k-cryptobench`, `zz9k-archive` | `C:` | Board/service introspection and release smoke check, plus accelerated image viewer, MP3 player, crypto-offload benchmark, and archive extractor (from zz9000-sdk). |
| ZZPlay | `ZZPlay` + `ZZPlay.info` | `SYS:Utilities/ZZ9000/` | MPEG-1 video and MP3 media player (from zz9000-sdk). See [the ZZ9000 drawer](#the-zz9000-drawer). |
| TLS offload | `amissl_v362.library` | `Libs:AmiSSL/` | AmiSSL 5.27 core with the ZZ9000 crypto-offload provider compiled in; accelerates supported TLS handshake and record crypto for all AmiSSL applications. Built per CPU (`68020-40` for 68020/030/040 and `68060`); the installer auto-detects the CPU and installs the matching build. Requires an existing AmiSSL 5.27 install. |
| Installer | `ZZ9000Installer` | Release zip root | Commodore Installer drawer used for end-user deployment. |

## The ZZ9000 drawer

ZZTop and ZZPlay are the two ZZ9000 tools with a Workbench interface, so
from v2.8 they share one drawer instead of being split between `SYS:Tools`
and `C:`:

```
SYS:Utilities/ZZ9000/ZZTop
SYS:Utilities/ZZ9000/ZZPlay
```

In Expert mode the installer asks where to create the drawer. It also
offers to add that drawer to the command path, because AmigaOS path
entries are not recursive — `SYS:Utilities` being on the path does not
make a drawer inside it searchable, and both tools have a CLI. Answering
yes writes a `Path ... ADD` line into a `;BEGIN ZZ9000` block in
`S:User-Startup`, which re-running the installer updates in place.

Upgrading from an earlier release, the installer offers to delete the old
`SYS:Tools/ZZTop` so you are not left running a stale copy.

## SD-Card Configuration (ZZ9000.CFG)

Firmware 2.3+ reads an optional `ZZ9000.CFG` file from the root of the
ZZ9000's FAT32 microSD card at cold boot (documented in the
[zz9000-firmware README](https://github.com/BlitterStudio/zz9000-firmware#configuration-file-zz9000cfg)).
ZZTop's **Project → Settings…** window reads and writes it directly
from AmigaOS, so the card never needs to leave the slot.

The drivers in this repo consult it too:

- `ZZ9000.card` takes its native-video defaults from the firmware's parsed
  configuration. ZZTop 2.8 presents one explicit **Native Output** profile
  instead of independent width, resolution, and refresh controls. Normal
  choices state the resulting resolution and refresh directly; capture
  sampling and framing live in **Advanced Video**. Framing defaults to
  **Automatic**: full-rate/full-width capture uses `280/40`, while filtered
  and Denise-adapter paths retain `188/26`. **Custom** values remain literal
  per-machine overrides. Firmware 2.8 with the profile capability stores this
  as `videocap_profile`; ZZTop transparently writes the equivalent legacy key
  combination for older firmware, including 2.8 RC1.
- With a matching v2.8-RC2-or-newer full-rate bitstream and firmware
  capability bit 3, `centered_1080p_60` places the unchanged 1280x1024 native
  content at `(320,28)` inside a 1920x1080 active raster. The content rectangle
  is `[320,1600) x [28,1052)` and every surrounding pixel stays black. This is
  native Amiga chipset video, not the separate Picasso96 1920x1080x32 RTG
  screen mode. The reused 150 MHz/2200x1125 timing produces approximately
  60.60606 Hz despite the nominal `60` profile name.
- The centered choice appears and is serialized only with matching support in
  the bitstream, firmware, `ZZ9000.card`, and ZZTop. Old, filtered-only, or
  mixed installations safely use `full_60`/native mode 1 and do not preserve a
  stored centered identity; `full_60` remains the default. Unrelated MAC and
  INT2 ENV overrides preserve a supported centered choice, while legacy
  native-video ENV overrides intentionally select their legacy fallback.
- `ZZ9000Net.device`, `zz9000ax.audio` and `mhizz9000.library` honor
  `int2 = on`; `ZZ9000Net.device` adopts the firmware's `mac`.
- `ZZ9000.card` also reads `offscreen_bitmaps` and `video_overlay`,
  the kill switches for card-side off-screen bitmaps and the P96 video
  window. Both default to on; ZZTop's Settings window edits them.

### Live native-video calibration

ZZTop 2.8 can calibrate Custom native-video framing without the old
guess/save/power-cycle loop. It requires matched firmware 2.8-or-newer and a
protocol-1 live-calibration bitstream. ZZTop checks an explicit firmware
live-control bit as well as the exact RTL capability, so either half of an
older/mixed install—including 2.8 RC1 firmware—leaves the existing Automatic
and numeric Custom controls available but keeps **Calibrate** disabled.

In **Project → Settings… → Advanced Video…**, leave the staged Native Output
path matching the currently applied path and choose **Calibrate…**. ZZTop opens
an explicit native PAL or NTSC Hires screen—never an RTG fallback—so its edge,
safe-area, and centre guides pass through the physical capture path being
adjusted.

- Arrow keys move the visible picture in the named direction, one crop unit at
  a time. Hold either Shift key for 16-unit steps.
- Values change on screen only after the FPGA applies the complete H/V control
  word at a capture-frame boundary and acknowledges it.
- **Enter** accepts the displayed pair as explicit Custom values and returns to
  Advanced Video. It does not write the SD card.
- **Escape** restores and acknowledges the exact state from calibration entry,
  including independent per-axis Automatic flags, before the native screen
  closes.
- **Done** stages an accepted preview in Settings; **Save** is the only action
  that writes `ZZ9000.CFG`. A cold boot later reproduces the saved pair.

Advanced Cancel, Settings Reload, and closing Settings restore their owning
live snapshots before discarding an unsaved preview. If an acknowledgement
times out, ZZTop keeps the relevant window open and reports that the state is
unknown; retry the restore when native frames return, or cold-boot to recover
the persisted CFG state. If another live writer wins the same request
sequence, ZZTop detects the different applied raw word and asks for a retry or
exact restore rather than claiming success. A Custom pair is tied to the
applied capture-path signature (sample mode plus full-width state). If staged
settings select a different path, Calibrate and Custom Save remain unavailable
until the path is restored, or framing is set to Automatic, saved,
cold-booted, and calibrated on the newly active path.

Precedence is always: `ENV:` variable (and RTG tooltypes) first, then
the config file, then the built-in default — so existing setups keep
working, but a lingering ENV variable also hides the config value.
Remove the ENV variables (`ZZ9K_INT2`, `ZZ9K_MAC`,
`ZZ9000-VCAP-800x600`, `ZZ9000-NS-VSYNC[-NTSC]`, `ZZ9000-NO-OFFSCREEN`,
`ZZ9000-NO-PIP`) when migrating to the config file. On firmware older than 2.3 the drivers silently fall back
to the ENV variables.

Activation follows the existing configuration lifecycle. ZZTop **Save** writes
the staged file, which is read at the next cold boot; valid runtime native-mode
writes take effect at the next stable vblank. The centered profile does not
add a monitor hot-plug or HDMI mode-switch guarantee.

## Command-Line Tools

### Firmware Updates

`ZZFwUpdate` copies a file from AmigaOS to the ZZ9000 FAT32 microSD card
without removing the card. The usual firmware update flow is:

```text
ZZFwUpdate RAM:BOOT.bin
```

Power-cycle the Amiga after replacing `BOOT.bin` so the ZZ9000 boots
the new firmware.

By default, the destination filename on the SD card is the source
basename. To write a different root-level filename, pass it as the
optional second argument:

```text
ZZFwUpdate SYS:Storage/zz9000-fw.bin BOOT.bin
```

The destination name must be 1-64 characters and contain only `A-Z`,
`a-z`, `0-9`, `.`, `_`, or `-`.

When you replace `BOOT.bin`, the firmware automatically keeps the
previous image as `BOOT.bak`. If a new firmware boots but misbehaves,
you can roll back to that backup without removing the card:

```text
ZZFwUpdate RESTORE
```

This promotes `BOOT.bak` to the active `BOOT.bin` (discarding the
replaced image, so no backup remains afterwards) after a confirmation
prompt. Pass `-y` to skip the prompt, or a name to restore something
other than `BOOT.bin`. Restore talks to the *running* firmware, so it
recovers a booting-but-misbehaving update; a fully non-booting card
still needs the microSD removed and restored on another computer.
Requires firmware with RESTORE (FWUP cmd 5) support.

### Network Diagnostics

`ZZNetStats` opens `ZZ9000Net.device`, requests SANA-II global stats,
and prints firmware RX queue/backpressure/drop counters:

```text
ZZNetStats
ZZNetStats DEVICE=Networks/ZZ9000Net.device UNIT=0
ZZNetStats Networks/ZZ9000Net.device 0
```

Run it before and after a throughput test to see whether drops are
happening in the Amiga-side driver or firmware RX path.

### Board Diagnostics

`ZZDiag` dumps the most useful hardware-facing diagnostics in one
place:

```text
ZZDiag
ZZDiag 3 50
```

The optional arguments are sample count and AmigaDOS delay ticks
between samples. The VideoCap section includes the detailed
video-capture and genlock diagnostic registers when the running
firmware exposes them.

### Scanlines

`ZZScanlines` controls the scanline bitstream modes exposed by recent
firmware/bitstream builds:

```text
ZZScanlines 0
ZZScanlines 1 0
ZZScanlines 2 1
ZZScanlines 3 0
```

Modes are `0=off`, `1=classic`, `2=soft`, `3=gradient`; parity is
`0=odd dark`, `1=even dark`.

Like ZZTop's Settings window, `ZZScanlines` changes the live FPGA
state; to make scanlines survive a power cycle, save them to
`ZZ9000.CFG` (firmware 2.3+, ZZTop Settings window's Save button).

## Building

GitHub Actions is the source of truth for release builds. Every push and
pull request builds each component inside
`sacredbanana/amiga-compiler:m68k-amigaos`; tag builds assemble the
release zip.

The same image can be used locally with Docker or Podman:

```bash
IMAGE=sacredbanana/amiga-compiler:m68k-amigaos
```

Top-level helpers mirror the CI build and package flow:

```bash
make build-all
make rtg-tests
make package-local
make check-release
make quality
```

Component `build.sh` wrappers also use the same Docker image when the
Amiga cross-toolchain is not already on `PATH`.

For exact commands for every artifact, use
[`tools/build-all.sh`](tools/build-all.sh) and
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

`sd-boot/zzsd.device` has a hard size ceiling of 7424 bytes because it
must fit in the FPGA-decoded boot ROM window. CI enforces this limit.

Release binaries are built by CI and should not be tracked in the source
tree unless there is a documented exception; see
[docs/BINARY_POLICY.md](docs/BINARY_POLICY.md).

## Release Packaging

Pushing a tag matching `v*` builds all artifacts and publishes a GitHub
Release zip:

```bash
git tag -a v2.3.0 -m "ZZ9000 drivers 2.3.0"
git push origin v2.3.0
```

The release bundle layout is:

```text
zz9000-drivers-<tag>/
  README.md
  ZZ9000Installer.info
  ZZ9000Installer/
```

The release job populates `ZZ9000Installer/` with fresh CI-built
binaries before zipping it. Binaries are not duplicated as loose files
at the zip root. The release zip's `README.md` is copied from
[installer/README.md](installer/README.md), which is focused on the
installer drawer layout and local installer testing.

Tags containing `-`, such as `v2.3.0-rc1`, are marked as pre-releases.
GitHub release notes are generated automatically.

## Repository Layout

| Path | Purpose |
|------|---------|
| `.github/workflows/ci.yml` | CI, artifact builds, release assembly. |
| `installer/` | End-user Commodore Installer drawer, icons, templates, and release packaging docs. |
| `rtg/` | Picasso96 RTG driver. |
| `net/` | SANA-II network driver and `ZZNetStats`. |
| `ZZDiag/` | Consolidated board and firmware diagnostics CLI. |
| `ahi/` | ZZ9000AX AHI driver, AudioMode file, and audio runtime documentation. |
| `mhi/` | ZZ9000AX MHI library. |
| `usb-poseidon/` | Poseidon USB hardware driver and setup notes. |
| `sd-boot/` | SD-card boot driver and boot-ROM integration notes. |
| `ZZTop/` | Configuration GUI. |
| `ZZScanlines/` | Scanline control CLI. |
| `ZZFwUpdate/` | Firmware/file push CLI using the FWUP protocol. |
| `common/` | Shared FWUP protocol client (`fwup_client.c`, `fwup_amiga.c`) and ZZ9000.CFG client (`zzcfg_amiga.c`) linked by `ZZFwUpdate` and `ZZTop`. |
| `sdk/` | Pulls the pinned zz9000-sdk ref and collects its end-user payloads (libraries, datatype, diagnostics). |
| `amissl/` | Builds the ZZ9000-accelerated `amissl.library` (adtools toolchain image + zz9000-sdk integration). |

## Credits

- RTG optimization work, scanline tooling, ZZTop V2/Settings updates,
  USB Poseidon driver, SD-card boot driver, firmware update/restore
  tool, network diagnostics, SDK runtime packaging, accelerated AmiSSL
  packaging, CI/release packaging, and installer modernization:
  Dimitris Panokostas (midwan).
- Scanline bitstream V1 and V2: Xanxi. V2 adds multi-mode patterns
  with odd/even parity, gated to AGA scandoubled modes and RTG
  resolutions below 350 lines.
- Network driver base: Henryk Richter <henryk.richter@gmx.net> (2018).
- Original upstream driver sources: MNT Research pre-fork repository
  listed above.

Per-file copyright notices are preserved in source files.

## License

SPDX-License-Identifier: `GPL-3.0-or-later`

See <https://spdx.org/licenses/GPL-3.0-or-later.html>.
