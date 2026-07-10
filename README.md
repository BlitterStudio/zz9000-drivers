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

## What This Fork Adds

Compared with the older MNT driver releases, this fork is now a full
installer-driven AmigaOS package for the current BlitterStudio firmware
and SDK stack:

- Commodore Installer packaging with Picasso96 settings migration,
  Zorro III high-memory settings, icons, rollback-safe AmiSSL backup,
  and CI-built binaries only.
- Native `BT_MNT_ZZ9000` Picasso96 identity, RTG fixes, 1920x1080x32
  settings for Zorro III, monitor DPMS power management, and clearer
  Zorro II/Zorro III profiles.
- `ZZTop` as the configuration GUI for firmware readback, FWUP
  update/restore, and SD-card `ZZ9000.CFG` editing.
- Poseidon USB hardware driver, SD-card boot support, firmware-file
  update/restore tooling, and board/network diagnostics.
- SDK runtime payloads (`zz9k.library`, picture datatype, `mpega.library`,
  SDK tools) built from the pinned `zz9000-sdk` revision.
- Accelerated AmiSSL packaging with CPU-specific `amissl.library` builds
  and the ZZ9000 OpenSSL provider compiled in.
- ZZ9000AX audio/MHI modernization and shared interrupt/config handling
  across AHI, MHI, and networking.

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

SDK offload services are limited on Zorro 2 boards: the CPU-visible
shared heap is a small window inside the 4 MB board aperture. Current
matched firmware, SDK runtime, and driver releases reserve a host-window
heap for audio/MP3 staging, so `mpega.library` and `mhizz9000.library`
work on Zorro 2. Do not mix older firmware or SDK payloads with current
drivers on Zorro 2, because the host-window/card-only allocation flags
must agree across the stack. Image decoding (`zz9k-picture.datatype`,
`zz9k-view`) and crypto offload (accelerated `amissl.library`) still
need Zorro 3 and transparently fall back to their software paths on
Zorro 2.

## Components

| Area | Artifact | Installed to | Notes |
|------|----------|--------------|-------|
| RTG graphics | `ZZ9000.card` | `Libs:Picasso96/` | Picasso96 RTG driver using the native `BT_MNT_ZZ9000` board identity, accelerated VRAM/PIP paths, and firmware-gated DPMS monitor power management. |
| P96 settings | `Picasso96Settings` / `Picasso96Settings-Z3` | `Devs:Picasso96Settings` | Installer backs up an existing file to `Devs:Picasso96Settings.pre-ZZ9000-2.4`, advertises Standby/Suspend/Active Off DPMS support, and offers a Zorro III high-memory profile with 1920x1080x32 enabled. |
| Networking | `ZZ9000Net.device` | `Devs:Networks/` | SANA-II Ethernet driver. |
| Network template | `ZZ9000Net` | `Devs:NetInterfaces/` | Optional Roadshow DHCP template installed by the installer. |
| Network diagnostics | `ZZNetStats` | `C:` | Dumps SANA-II counters plus firmware RX backlog/drop registers. |
| Board diagnostics | `ZZDiag` | `C:` | Dumps board identity, firmware, VideoCap, USB, SD, AX/audio, and Ethernet diagnostic registers. |
| AHI audio | `zz9000ax.audio` | `Devs:AHI/` | ZZ9000AX AHI driver. Runtime tunables are documented in [ahi/README.md](ahi/README.md). |
| AHI mode | `ZZ9000AX` | `Devs:AudioModes/` | AudioMode file generated by the AHI build. |
| MHI audio | `mhizz9000.library` | `Libs:MHI/` | Exposes the AX hardware MP3 decoder to MHI-aware players. |
| USB | `zzusbhw.device` | `Devs:USBHardware/` | Poseidon USB hardware driver. See [usb-poseidon/README.md](usb-poseidon/README.md). |
| SD boot | `zzsd.device` | Firmware `BOOT.bin` | Size-constrained boot driver for FAT32-hosted HDF boot. See [sd-boot/README.md](sd-boot/README.md). |
| Configuration | `ZZTop` | `SYS:Tools/` | GUI for hardware readback, firmware update/restore, and the SD-card `ZZ9000.CFG` settings (Project menu > Settings: native video mode, exact refresh, scanlines, INT2, MAC, boot HDF; needs firmware 2.3+). |
| Scanlines | `ZZScanlines` | `C:` | CLI for scanline V1/V2 modes. |
| Firmware update | `ZZFwUpdate` | `C:` | Pushes `BOOT.bin` or another root-level file to the ZZ9000 FAT32 microSD card over Zorro. |
| SDK services | `zz9k.library` | `Libs:` | AmigaOS gateway to the SDK v2 firmware services (image decode, audio, compression, crypto). Built from the pinned [zz9000-sdk](https://github.com/BlitterStudio/zz9000-sdk) ref by `sdk/build.sh`. |
| MP3 decode | `mpega.library` | `Libs:` | ARM-accelerated drop-in MPEGA replacement (from zz9000-sdk). |
| Picture datatype | `zz9k-picture.datatype` | `SYS:Classes/DataTypes/` | Hardware-accelerated picture datatype; JPEG/PNG descriptors staged inactive in `SYS:Storage/DataTypes` (from zz9000-sdk). |
| SDK tools | `zz9k-info`, `zz9k-services`, `zz9k-view`, `zz9k-mp3`, `zz9k-cryptobench`, `zz9k-archive` | `C:` | Board/service introspection and release smoke check, plus the accelerated image viewer, MP3 player, crypto-offload benchmark, and archive extractor (from zz9000-sdk). |
| TLS offload | `amissl_v362.library` | `Libs:AmiSSL/` | AmiSSL 5.27 core with the ZZ9000 crypto-offload provider compiled in; accelerates supported TLS handshake and record crypto for all AmiSSL applications. Built per CPU (`68020-40` for 68020/030/040 and `68060`); the installer auto-detects the CPU and installs the matching build. Requires an existing AmiSSL 5.27 install. |
| Installer | `ZZ9000Installer` | Release zip root | Commodore Installer drawer used for end-user deployment. |

## SD-Card Configuration (ZZ9000.CFG)

Firmware 2.3+ reads an optional `ZZ9000.CFG` file from the root of the
ZZ9000's FAT32 microSD card at cold boot (documented in the
[zz9000-firmware README](https://github.com/BlitterStudio/zz9000-firmware#configuration-file-zz9000cfg)).
ZZTop's **Project → Settings…** window reads and writes it directly
from AmigaOS, so the card never needs to leave the slot.

The drivers in this repo consult it too:

- `ZZ9000.card` takes its videocap mode and non-standard-vsync
  defaults from `videocap_mode` / `nonstandard_vsync`. With neither
  ENV nor config set, native video now defaults to 800x600 @ 60 Hz —
  the mode most monitors accept, and what the firmware and ZZTop
  default to (older `ZZ9000.card` versions forced 720x576 @ 50 Hz).
  PAL-capable setups select `videocap_mode = pal` in ZZTop's Settings
  window.
- `ZZ9000Net.device`, `zz9000ax.audio` and `mhizz9000.library` honor
  `int2 = on`; `ZZ9000Net.device` adopts the firmware's `mac`.

Precedence is always: `ENV:` variable (and RTG tooltypes) first, then
the config file, then the built-in default — so existing setups keep
working, but a lingering ENV variable also hides the config value.
Remove the ENV variables (`ZZ9K_INT2`, `ZZ9K_MAC`,
`ZZ9000-VCAP-800x600`, `ZZ9000-NS-VSYNC[-NTSC]`) when migrating to the
config file. On firmware older than 2.3 the drivers silently fall back
to the ENV variables.

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
