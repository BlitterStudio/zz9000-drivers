<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 Commodore Installer

This is a Commodore Installer script drawer that end users run on an
AmigaOS system to install the ZZ9000's drivers, tools and config
files. It's bundled into every GitHub Release zip.

## Layout

```
installer/
├── ZZ9000Installer.info              Drawer icon
└── ZZ9000Installer/
    ├── Install ZZ9000                Installer script (what the user double-clicks)
    ├── Install ZZ9000.info           Script icon
    ├── Tools.info                    Sub-drawer icon
    ├── Docs/
    │   ├── ahi-README.md             Audio tunables doc populated by CI
    │   ├── usb-poseidon-README.md    Poseidon setup doc populated by CI
    │   ├── sdk-README.md             SDK runtime doc populated by CI
    │   └── amissl-README.md          Accelerated AmiSSL doc populated by CI
    ├── Devs/
    │   ├── Picasso96Settings         Zorro II-safe P96 screenmode config  (committed)
    │   ├── Picasso96Settings-Z3      Zorro III high-memory P96 config  (committed)
    │   ├── USBHardware/              ← zzusbhw.device populated by CI
    │   ├── NetInterfaces/
    │   │   └── ZZ9000Net             Roadshow NetInterface template  (committed)
    │   ├── AHI/                      ← zz9000ax.audio populated by CI
    │   ├── AudioModes/               ← ZZ9000AX populated by CI
    │   └── Networks/                 ← ZZ9000Net.device populated by CI
    ├── Classes/
    │   └── DataTypes/               ← zz9k-picture.datatype populated by CI
    ├── Storage/
    │   └── DataTypes/               ← JPEG/PNG datatype descriptors populated by CI
    ├── Libs/
    │   ├── MHI/                      ← mhizz9000.library populated by CI
    │   ├── Picasso96/                ← ZZ9000.card populated by CI
    │   ├── zz9k.library             ← populated by CI
    │   ├── mpega.library            ← populated by CI
    │   └── AmiSSL/
    │       ├── 68020-40/            ← amissl_v362.library (68020/030/040) populated by CI
    │       └── 68060/               ← amissl_v362.library (68060) populated by CI
    └── Tools/
        ├── ZZTop.info                Icon  (committed)
        └──                           ← ZZTop, ZZScanlines, ZZFwUpdate,
                                        ZZNetStats, ZZDiag,
                                        zz9k-info, zz9k-services, zz9k-view,
                                        zz9k-mp3, zz9k-cryptobench, zz9k-archive
                                        populated by CI
```

Only **icons and configuration templates** are committed. Every binary
payload is built by CI and copied into this tree during the release
assembly step (see `.github/workflows/ci.yml`, `release` job). The
`.gitignore` rules on `*.card`, `*.device`, `*.audio`, etc. keep any
locally-populated binaries from accidentally being committed back.

This means a fresh `git clone` will **not** produce a runnable
Installer drawer on its own — CI must have populated it, or you must
copy the binaries in by hand.

`ZZ9000.card` 2.4 and newer uses the native `BT_MNT_ZZ9000` Picasso96
board type. Upgrade tests should copy one of the committed
`ZZ9000Installer/Devs/Picasso96Settings*` files along with the new RTG
driver; old settings saved under the legacy `uaegfx` board type are not
guaranteed to attach to the new driver identity. The default
`Picasso96Settings` profile is safe for Zorro II and Zorro III. The
`Picasso96Settings-Z3` profile enables 1920x1080x32 and should only be
installed on Zorro III systems. Both profiles advertise the monitor's
Picasso96 DPMS Standby, Suspend, and Active Off capabilities so DPMS
utilities can reach the firmware-gated `ZZ9000.card` callback.
The installer backs up the previous settings file as
`Devs:Picasso96Settings.pre-ZZ9000-2.4` before installing the migrated
settings file.

The SDK runtime payloads (`zz9k.library`, the ARM-accelerated
`mpega.library` drop-in, the `zz9k-picture.datatype` plus its inactive
JPEG/PNG descriptors under `Storage/DataTypes`, and the `zz9k-#?` CLI
tools) are built from the pinned zz9000-sdk ref and populated by CI.
They need the SDK-service ZZ9000 firmware to do anything useful. On
Zorro 2 boards, current matched firmware, SDK payloads, and drivers use
host-window-aware audio/MP3 staging buffers for `mpega.library` and
MHI. Image decoding and crypto offload need the Zorro 3 board window and
fall back to software there (the installer prompts say so too). Keep the
firmware, SDK payloads, and drivers from the same release set when using
Zorro 2 SDK audio paths.

When installing the ZZ9000-accelerated `amissl.library`, the installer
first backs up the stock `LIBS:AmiSSL/amissl_v362.library` to
`amissl_v362.library.bak` — only if no `.bak` already exists, so
re-running the installer always preserves the genuine original — then
installs the build matching the host CPU (`Libs/AmiSSL/68020-40/` for
68020/030/040 and the Apollo 68080, `Libs/AmiSSL/68060/` for the 68060),
detected exactly as AmiSSL's own installer does. To revert, delete the
accelerated `amissl_v362.library` and rename the `.bak` back. The
accelerated library covers supported TLS handshake and record crypto;
unsupported algorithms automatically stay on AmiSSL's software provider.

## Trying the installer locally

If you want to test the installer with your own local builds (rather
than downloading a release zip), copy each artifact into the drawer
using the same layout CI uses:

```sh
INST=installer/ZZ9000Installer
install -Dm644 rtg/ZZ9000.card                 "$INST/Libs/Picasso96/ZZ9000.card"
install -Dm644 mhi/mhizz9000.library           "$INST/Libs/MHI/mhizz9000.library"
install -Dm644 usb-poseidon/zzusbhw.device     "$INST/Devs/USBHardware/zzusbhw.device"
install -Dm644 net/ZZ9000Net.device            "$INST/Devs/Networks/ZZ9000Net.device"
install -Dm644 ahi/driver/zz9000ax.audio       "$INST/Devs/AHI/zz9000ax.audio"
install -Dm644 ahi/driver/ZZ9000AX             "$INST/Devs/AudioModes/ZZ9000AX"
install -Dm755 ZZTop/ZZTop                     "$INST/Tools/ZZTop"
install -Dm755 ZZScanlines/ZZScanlines         "$INST/Tools/ZZScanlines"
install -Dm755 ZZFwUpdate/ZZFwUpdate           "$INST/Tools/ZZFwUpdate"
install -Dm755 net/ZZNetStats/ZZNetStats       "$INST/Tools/ZZNetStats"
install -Dm755 ZZDiag/ZZDiag                   "$INST/Tools/ZZDiag"
install -Dm644 ahi/README.md                   "$INST/Docs/ahi-README.md"
install -Dm644 usb-poseidon/README.md          "$INST/Docs/usb-poseidon-README.md"
```

The SDK runtime payloads and the per-CPU accelerated
`amissl_v362.library` are not produced by the lines above: they come
from `sdk/out` and `amissl/out` after their own builds. The easiest way
to stage everything (drivers, SDK payloads, and both amissl builds) into
the drawer exactly as CI does is `make package-local`, which runs
`tools/package-local.sh`.

Then copy the `installer/` tree onto your Amiga and double-click
`Install ZZ9000`.

For a direct RTG-only test, copy `rtg/ZZ9000.card` to
`Libs:Picasso96/` and copy
`installer/ZZ9000Installer/Devs/Picasso96Settings` to `Devs:`.

## Release bundle layout

The CI release job (tag push `v*`) produces a zip with the installer
contents at the package root:

```
zz9000-drivers-<tag>/
├── README.md
├── ZZ9000Installer.info
└── ZZ9000Installer/
```

Driver and tool binaries are not duplicated as loose files at the zip
root; they live in the exact drawer paths consumed by `Install ZZ9000`.

The release job also copies component docs into `ZZ9000Installer/Docs/`:
`ahi-README.md` covers the three audio ENV tunables
(`ZZ9K_MIX_LEVELS`, `ZZ9000AX-NOLPF`, `ZZ9K_INT2`),
`usb-poseidon-README.md` covers Poseidon registration and
troubleshooting, `sdk-README.md` documents the SDK runtime payloads,
and `amissl-README.md` documents the accelerated AmiSSL build.

## What doesn't go through this Installer

The CI-built artifact below is **not** handled by this Installer script
because it doesn't live on the AmigaOS filesystem:

| Artifact            | Install path                                                   |
|---------------------|----------------------------------------------------------------|
| `zzsd.device`       | Packed into firmware `BOOT.bin` — see [sd-boot/README.md](../sd-boot/README.md). |

## Updating the script

- The script lives at `installer/ZZ9000Installer/Install ZZ9000` and
  is plain text (Commodore Installer LISP-like syntax).
- If you add a new artifact, add both:
  - A `(copyfiles ...)` step in the script with the right `(source ...)`
    path inside the drawer and `(dest ...)` on the target filesystem.
  - A matching `install -Dm...` line in the `release` job of
    `.github/workflows/ci.yml` so the binary actually lands in the
    drawer before zipping.
