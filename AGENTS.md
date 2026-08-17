# AGENTS.md

## What This Repo Is

AmigaOS drivers and tools for the MNT ZZ9000 Zorro II/III hardware card
(BlitterStudio fork). C code targeting m68k-AmigaOS. Matching FPGA logic and ARM
firmware live in `zz9000-firmware`; reusable libraries and tools come from the
exact `zz9000-sdk` commit pinned in `sdk/SDK_REF`.

**License**: GPL-3.0-or-later

## Build System

All Amiga cross-builds use Docker/Podman with the cross-compilation image:
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

### Matched stack and pinned SDK

- Firmware, bitstreams, drivers, and the SDK payload are one release set when a
  change affects registers, capabilities, mode identities, mailbox services, or
  shared-memory layouts. Mixed-version fallbacks must remain safe, but are not a
  substitute for testing and packaging the matched set.
- `sdk/SDK_REF` deliberately pins one exact, publicly reachable SDK commit. Do
  not silently build a sibling SDK checkout or advance the pin without the
  corresponding payload, tests, documentation, and installer integration.
- Keep installer payload names, drawer placeholders, README guidance, and
  release checks aligned whenever an artifact is added, renamed, or removed.

### `ZZ9000.CFG` and ZZTop

- The firmware parser is the source of truth for accepted keys. ZZTop rewrites
  the file from its own model, so the canonical keys and ordered
  `videocap_profile` values must stay aligned across the firmware parser,
  firmware sample, firmware README, and `common/zzcfg_amiga.c`. Run
  `tools/check-cfg-keys.sh [path-to-zz9000-firmware]` after either side changes.
- Profile ordering is an append-only shared schema. Do not reorder or reuse a
  numeric identity. The legacy `videocap_mode`, `videocap_shres`, and
  `nonstandard_vsync` keys remain read compatibility only; new UI and docs use
  the atomic profile.
- ZZTop's capability and legacy label arrays must remain index-aligned with the
  shared mode enum. Add the widest new cycle label to `settings_value_samples`
  or the value gadget will clip it even when the real strings are correct.

### Video-capture mode identity

- Keep the explicit video-capture mode/profile identity in RTG `CardData`; a
  boolean “native” flag cannot distinguish the two 1280×1024 full-detail modes.
- Runtime mode 5 means the exact centered 1280×1024 viewport only when firmware
  advertises `ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P` (firmware publishes that only
  for the complete viewport-layout and dynamic-clock path). Older or mismatched
  cards must sanitize it to the established full-frame 60 Hz mode. Modes 1 and
  6 retain their existing full-frame behavior.
- The centered profile is exposed only by the full-rate firmware variants. Do
  not infer support from the numeric mode alone or advertise it on A500-family
  variants without new hardware qualification.

### Zorro II aperture contract

- Current Zorro II software uses the generation-1 aperture descriptor. RTG must
  validate the published 2 MiB or 4 MiB layout against AutoConfig, reserve every
  region, and only then acknowledge it. Invalid or unacknowledged descriptors
  fail closed.
- CPU-visible SDK allocations share one 64 KiB host window; it is not 64 KiB per
  process. The shipped 4 MiB profile has one fixed 224 KiB PIP pool, while the
  2 MiB profile has no PIP pool. Do not restore fixed end-of-board service
  addresses or advertise arbitrary Picasso96 off-screen allocations.
- Descriptor-absent legacy 4 MiB cards retain only the historical fixed host
  window; legacy 2 MiB and unknown aperture sizes reject it. Not every low-level
  diagnostic is Zorro II-safe—check the production-client matrix in the pinned
  SDK's `docs/zz9k-zorro2-services.md` before expanding support claims.

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
- **Cross-repo CFG guard**: `tools/check-cfg-keys.sh ../zz9000-firmware` checks canonical keys and append-only profile identities against the firmware checkout.
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
procedures stay in the SDK repo). Their CI jobs need that exact commit to be
public and cloneable before a drivers release is tagged.

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
