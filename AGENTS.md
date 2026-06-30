# AGENTS.md

## What This Repo Is

AmigaOS drivers and tools for the MNT ZZ9000 Zorro II/III hardware card (BlitterStudio fork). C code targeting m68k-AmigaOS. Matching FPGA logic and ARM firmware live in `zz9000-firmware`.

**License**: GPL-3.0-or-later

## Build System

All builds use Docker/Podman with the cross-compilation image:
```
sacredbanana/amiga-compiler:m68k-amigaos
```

The `tools/amiga-docker.sh` wrapper handles engine detection (docker or podman), volume mounting, and PATH setup. Component `build.sh` scripts auto-fall back to this wrapper when the toolchain is not on PATH.

### Key Commands

| Command | What It Does |
|---------|-------------|
| `make build-all` | Build all components sequentially via `tools/build-all.sh` |
| `make rtg-tests` | Run RTG unit tests (`rtg/tests/`) |
| `make quality` | Run shellcheck, actionlint, cppcheck (graceful skip if missing) |
| `make package-local` | Assemble local release zip via `tools/package-local.sh` |
| `make check-release` | Verify all artifacts exist, no binaries tracked in git |

Individual component targets: `make rtg`, `make zztop`, `make net`, `make ahi`, `make mhi`, `make usb-poseidon`, `make sd-boot`, etc. See Makefile for the full list.

### CI is Source of Truth

`.github/workflows/ci.yml` builds every component in parallel jobs. Each job uses Docker directly (not the wrapper). Tag pushes (`v*`) trigger the release job that assembles and publishes a zip. Tags containing `-` become pre-releases.

## Critical Constraints

- **sd-boot/zzsd.device hard size ceiling: 7423 bytes** (must fit in FPGA-decoded boot ROM window). CI enforces `< 7424`. The `check-release.sh` script also checks this.
- **No generated binaries in git**. Build outputs (*.card, *.device, *.audio, *.library, executables) are `.gitignore`d and enforced by `tools/check-release.sh` and `tools/tests/test_repo_tooling.py`. If you add a new tool, add its output to `.gitignore` AND create an empty placeholder in the installer drawer so packaging doesn't accidentally stage stale binaries.
- **Shared headers**: `include/zz9000_hw.h` (hardware registers) and `include/zz9000_ax.h` (AX audio constants). Small tools MUST include these rather than duplicating definitions. The test suite enforces this.

## Repository Structure

| Directory | Artifact(s) | Build Command |
|-----------|------------|---------------|
| `rtg/` | `ZZ9000.card` (Picasso96 RTG driver) | `./build.sh` or single gcc invocation |
| `net/` | `ZZ9000Net.device` (SANA-II network) | `make` (internal Makefile) |
| `net/ZZNetStats/` | `ZZNetStats` (network diagnostics CLI) | Single gcc invocation |
| `ahi/driver/` | `zz9000ax.audio`, `ZZ9000AX` (AHI audio) | `./build.sh` |
| `mhi/` | `mhizz9000.library` (MHI MP3 decoder) | `./build.sh` |
| `usb-poseidon/` | `zzusbhw.device` (USB hardware driver) | `./build.sh` or single gcc invocation |
| `sd-boot/` | `zzsd.device` (SD-card boot, size-constrained) | `./build.sh` |
| `ZZTop/` | `ZZTop` (configuration GUI) | `./build-gcc.sh` |
| `ZZScanlines/` | `ZZScanlines` (scanline control CLI) | Single gcc invocation |
| `ZZFwUpdate/` | `ZZFwUpdate` (firmware push CLI) | Single gcc invocation |
| `ZZDiag/` | `ZZDiag` (board diagnostics CLI) | `./build.sh` |
| `sdk/` | `zz9k.library`, `mpega.library`, `zz9k-picture.datatype`, `zz9k-info`, `zz9k-services` (pulled from the pinned zz9000-sdk ref) | `sdk/build.sh` (host-side; drives the SDK's own Docker build) |
| `amissl/` | `amissl_v362.library` (ZZ9000-accelerated AmiSSL core) | `amissl/build.sh` (host-side; adtools image, slow — not part of build-all) |
| `installer/` | Commodore Installer drawer, icons, templates | Populated by CI/release scripts |

## Testing

- **RTG unit tests**: `make rtg-tests` runs C tests in `rtg/tests/` (host-native compilation).
- **Repo tooling tests**: `python3 -m unittest tools/tests/test_repo_tooling.py` validates build scripts, CI config, binary tracking, shared header usage, and audio driver invariants.
- **Quality gates** (`make quality`): shellcheck on all `.sh` files, actionlint on CI workflow, cppcheck on select C files. All gracefully skip if the tool is missing.

CI runs `host-checks` (rtg-tests + Python tests + quality + quick release check) in parallel with component builds.

## Release Flow

1. Build all artifacts: `make build-all` (plus `amissl/build.sh` for the
   accelerated AmiSSL library — slow, so not part of build-all)
2. Verify: `tools/check-release.sh` (full, not --quick)
3. Tag and push: `git tag -a v2.x.y -m "..." && git push origin v2.x.y`
4. CI builds everything in parallel, assembles the installer drawer, creates a GitHub Release zip

The `sdk/` and `amissl/` components consume the zz9000-sdk repository at the
commit pinned in `sdk/SDK_REF` ("pull, not move" — sources, tests, and smoke
procedures stay in the SDK repo). Their CI jobs need that repository to be
public/cloneable.

Local packaging alternative: `make package-local` (runs check-release first, then zips).

## Shell Script Conventions

- All scripts use `#!/bin/sh` + `set -eu`.
- Directory resolution uses `CDPATH='' cd -- "$(dirname -- "$0")" && pwd` pattern.
- PATH expansion for the Amiga toolchain must happen inside the container, not on the host (see `tools/amiga-docker.sh` SC2016 comment).

## Compiler Flags by Component

Components use different m68k CPU targets and optimization levels:
- RTG (`ZZ9000.card`): `-m68020 -mtune=68020-60 -O2 -fomit-frame-pointer`
- ZZTop, ZZDiag: `-m68030 -O2`
- USB Poseidon: `-m68020 -mtune=68020-60 -msoft-float -Os` (nostdlib)
- SD Boot: `-m68000 -Os` (nostdlib, size-critical)
- ZZScanlines, ZZFwUpdate, ZZNetStats: `-O2` (noixemul, standard libs)

Match existing flags when modifying build commands. CI is the canonical reference.
