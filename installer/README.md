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
    │   └── usb-poseidon-README.md    Poseidon setup doc populated by CI
    ├── Devs/
    │   ├── Picasso96Settings         P96 screenmode config  (committed)
    │   ├── USBHardware/              ← zzusbhw.device populated by CI
    │   ├── NetInterfaces/
    │   │   └── ZZ9000Net             Roadshow NetInterface template  (committed)
    │   ├── AHI/                      ← zz9000ax.audio populated by CI
    │   ├── AudioModes/               ← ZZ9000AX populated by CI
    │   └── Networks/                 ← ZZ9000Net.device populated by CI
    ├── Libs/
    │   ├── MHI/                      ← mhizz9000.library populated by CI
    │   └── Picasso96/                ← ZZ9000.card populated by CI
    └── Tools/
        ├── ZZTop.info                Icon  (committed)
        ├── axmp3.info                Icon  (committed)
        └──                           ← ZZTop, ZZScanlines, zznetstats, axmp3
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
board type. Upgrade tests should copy the committed
`ZZ9000Installer/Devs/Picasso96Settings` file along with the new RTG
driver; old settings saved under the legacy `uaegfx` board type are not
guaranteed to attach to the new driver identity.
The installer backs up the previous settings file as
`Devs:Picasso96Settings.pre-ZZ9000-2.4` before installing the migrated
settings file.

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
install -Dm755 net/zznetstats/zznetstats       "$INST/Tools/zznetstats"
install -Dm755 ax-direct/axmp3                 "$INST/Tools/axmp3"
install -Dm644 ahi/README.md                   "$INST/Docs/ahi-README.md"
install -Dm644 usb-poseidon/README.md          "$INST/Docs/usb-poseidon-README.md"
```

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
(`ZZ9K_MIX_LEVELS`, `ZZ9000AX-NOLPF`, `ZZ9K_INT2`), and
`usb-poseidon-README.md` covers Poseidon registration and
troubleshooting.

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
