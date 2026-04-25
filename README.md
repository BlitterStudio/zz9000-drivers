[![CI](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml/badge.svg)](https://github.com/BlitterStudio/zz9000-drivers/actions/workflows/ci.yml)

# ZZ9000 Drivers (AmigaOS) — BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 AmigaOS driver sources. It is
> maintained by Dimitris Panokostas / **BlitterStudio** and is **not
> affiliated with, endorsed by, or supported by MNT Research GmbH**.
> The ZZ9000 hardware itself is designed and manufactured by MNT
> Research GmbH — hardware questions belong with them; driver issues
> and fork-specific discussion belong in this repo's
> [Issues](https://github.com/BlitterStudio/zz9000-drivers/issues).
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-drivers

AmigaOS driver set for the MNT ZZ9000 Zorro II/III card: RTG graphics,
SANA-II networking, AHI/MHI audio for the ZZ9000AX daughterboard, USB
(Poseidon), SD-card boot from a FAT32-hosted HDF, plus the ZZTop
configuration GUI and a scanline CLI. Everything targets `m68k-amigaos`
and builds headless via Docker. The matching FPGA logic and ARM
firmware live in
[zz9000-firmware](https://github.com/BlitterStudio/zz9000-firmware).

## What this fork adds on top of upstream

- **RTG driver optimizations** — SetColorArray Z3 batch palette path,
  AllocBitMap improvements, general Picasso96 compatibility hardening.
- **USB Poseidon driver** (`zzusbhw.device`) — chunked bulk transfers,
  root-hub emulation, async INT via poll task, Z3 autoconfig preference,
  CopyMemQuick fast path, mailbox protocol. Paired with the firmware's
  new USB stack.
- **SD-card boot driver** (`zzsd.device`) — boots AmigaOS from a
  hardfile on a FAT32 SD card via the autoboot ROM. Size-constrained
  (7424 bytes); CI enforces the ceiling.
- **ZZScanlines V2** — CLI front-end for the multi-mode scanline
  bitstream (classic / soft / gradient with parity control).
- **ZZTop V2** — config GUI with scanline slider, hardware readback,
  and the new toggles exposed by firmware 2.0.0.
- **CI + releases** — GitHub Actions builds every component on push/PR
  and publishes a tagged release zip on `v*` tags
  ([.github/workflows/ci.yml](.github/workflows/ci.yml)).

## Components

| Folder           | Artifact                 | What it does |
|------------------|--------------------------|--------------|
| `rtg/`           | `ZZ9000.card`            | Picasso96-compatible RTG graphics driver (not P96-derived). Installs under `Libs:Picasso96`. |
| `net/`           | `ZZ9000Net.device`       | SANA-II network driver. Installs under `Devs:Networks`. |
| `ahi/`           | `zz9000ax.audio`         | AHI audio driver for the ZZ9000AX daughterboard. `ahi/axtest/` has standalone tests. Runtime tunables (mixer balance, LPF, INT2) are documented in [ahi/README.md](ahi/README.md). |
| `ax-direct/`     | `axtest`, `axmp3`        | Direct-register reference tools for the AX audio subsystem — bringup and hardware MP3 playback. |
| `mhi/`           | `mhizz9000.library`      | MHI library exposing the AX hardware MP3 decoder to MHI-aware players. Shares the AX card (and its ENV tunables) with the AHI driver — see [ahi/README.md](ahi/README.md). |
| `usb-poseidon/`  | `zzusbhw.device`         | USB hardware driver for Poseidon — chunked bulk transfers, root-hub emulation, async INT via poll task, mailbox protocol. Paired with the ZZ9000OS firmware USB stack. |
| `sd-boot/`       | `zzsd.device`            | Boots AmigaOS from a hardfile (`/zz9000.hdf`) on a FAT32 SD card, via the ZZ9000's autoboot ROM. See [sd-boot/README.md](sd-boot/README.md). |
| `ZZTop/`         | `ZZTop`                  | Configuration GUI (resolution, scanlines, toggles, hardware readback). |
| `ZZScanlines/`   | `ZZScanlines`            | CLI front-end for the V1/V2 scanline bitstream. |
| `installer/`     | `ZZ9000Installer`        | Commodore Installer script and icon for end-user deployment. |

## Building

Every component has a `build.sh` in its folder, and
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds all of
them on every push and pull request. The simplest local path is the
`sacredbanana/amiga-compiler:m68k-amigaos` Docker image, which ships a
working `vbcc` + `m68k-amigaos-gcc` toolchain — the same image the
CI uses.

RTG driver:

```bash
docker run --rm -v "$(pwd)":/src -w /src/rtg \
  sacredbanana/amiga-compiler:m68k-amigaos \
  sh -c 'export PATH=/opt/amiga/bin:$PATH && sh build.sh'
```

ZZTop:

```bash
docker run --rm -v "$(pwd)":/src -w /src/ZZTop \
  sacredbanana/amiga-compiler:m68k-amigaos \
  m68k-amigaos-gcc Sources/ZZTop.c -m68030 -O2 -o ZZTop -Wall -Wextra \
  -Wno-unused-parameter -lamiga -noixemul -lm
```

ZZScanlines:

```bash
docker run --rm -v "$(pwd)":/src -w /src/ZZScanlines \
  sacredbanana/amiga-compiler:m68k-amigaos \
  m68k-amigaos-gcc -O2 -noixemul -o ZZScanlines ZZScanlines.c -lamiga
```

For the network, AHI, MHI, USB Poseidon and SD-boot drivers, run the
folder's `build.sh` (or copy the exact command from
[`.github/workflows/ci.yml`](.github/workflows/ci.yml)).
`zzsd.device` has a hard size ceiling of **7424 bytes** (FPGA-decoded
boot ROM window minus the diag-area header and thunk) — CI enforces
this.

## Releases

Pushing a tag matching `v*` (e.g. `v1.0.0`, `v2026.04`) triggers the
full CI build and then publishes `zz9000-drivers-<tag>.zip` containing
`README.md`, `ZZ9000Installer.info`, and the populated
`ZZ9000Installer/` drawer at the package root. Built binaries are
placed inside the installer drawer and are not duplicated as loose
files at the zip root. Tags containing a `-` (e.g. `v1.0.0-rc1`) are
marked as pre-releases. Release notes are generated automatically from
commits and merged PRs since the previous tag.

```bash
git tag -a v1.0.0 -m "ZZ9000 drivers 1.0.0"
git push origin v1.0.0
```

## Installing on a real ZZ9000

The easiest path is the Commodore Installer bundled in `installer/` (or
inside the release zip) — it handles file placement, icons, and the
Picasso96 entries for you. That's the recommended route for end users.

For manual installs or component-by-component replacement:

| Artifact                          | Destination             |
|-----------------------------------|-------------------------|
| `rtg/ZZ9000.card`                 | `Libs:Picasso96/`       |
| `net/ZZ9000Net.device`            | `Devs:Networks/`        |
| `ahi/driver/zz9000ax.audio`       | `Devs:AHI/`             |
| `mhi/mhizz9000.library`           | `Libs:MHI/`             |
| `usb-poseidon/zzusbhw.device`     | `Devs:USBHardware/` then registered with Poseidon |
| `sd-boot/zzsd.device`             | Packed into `BOOT.bin` — see [sd-boot/README.md](sd-boot/README.md) |

## Credits

- RTG driver optimizations (SetColorArray Z3 batch path, AllocBitMap),
  scanline tooling (ZZScanlines V2 port, ZZTop V2 slider with hardware
  readback), USB Poseidon hardware driver (`zzusbhw.device`, chunked
  bulk transfers, root-hub emulation, async INT via poll task, Z3
  autoconfig preference, CopyMemQuick fast path, mailbox protocol,
  throughput tuning), and SD-card boot driver (`zzsd.device`) —
  Dimitris Panokostas (midwan).
- Scanline bitstream V1 and V2 — Xanxi. V2 adds multi-mode patterns
  (classic / soft / gradient) with odd/even parity, gated to AGA
  scandoubled modes and RTG resolutions below 350 lines. `ZZScanlines.c`
  V2 is essentially Xanxi's reference implementation, adapted for this
  repo's conventions.
- Network driver originally based on work by Henryk Richter
  <henryk.richter@gmx.net> (2018).
- Upstream driver sources (pre-fork) — see the fork notice above.

Per-file copyright notices are preserved in each source file.

## License

SPDX-License-Identifier: **GPL-3.0-or-later**
<https://spdx.org/licenses/GPL-3.0-or-later.html>
