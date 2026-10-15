<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 AmigaOS Installer

The `ZZ9000Installer` drawer in a driver release installs the ZZ9000 drivers,
tools, manuals, and optional SDK and AmiSSL payloads on AmigaOS. Double-click
`Install ZZ9000` after unpacking the release LHA. The script puts each file in
its AmigaOS destination, preserves selected existing files, and offers the
choices described below.

## Install a release

1. Unpack the release LHA on the Amiga and open `ZZ9000Installer`.
2. Double-click `Install ZZ9000`.
3. For RTG support, install Picasso96 first and select either the `Altais` or
   `ZZ9000` monitor. The script stops when `Libs:Picasso96/rtg.library` is
   older than version 40 or neither monitor file is present.
4. Choose the Zorro II-safe profile unless the machine has a Zorro III
   ZZ9000. The Zorro III profile enables the 1920x1080x32 screen mode.
5. Answer the optional USB, SDK, AmiSSL, ZZCapture, and command-path prompts
   as needed.

At Installer user level 2 or higher, the script asks where to create the
`ZZ9000` program drawer and where to put general manuals. The defaults are
`SYS:Utilities/ZZ9000` and that same drawer. `ZZTop`, `ZZPlay`, and their
manuals always go in the program drawer.

## What the script installs

| Component | Source in `ZZ9000Installer/` | Destination and behaviour |
| --- | --- | --- |
| Picasso96 driver | `Libs/Picasso96/ZZ9000.card` | Offers to install or replace `Libs:Picasso96/ZZ9000.card`. |
| Monitor and P96 settings | Existing `Devs:Monitors/Altais`; `Devs/Picasso96Settings` or `Devs/Picasso96Settings-Z3` | Creates `Devs:Monitors/ZZ9000` from `Altais` when needed, removes the old `Altais` file after confirmation, sets `BOARDTYPE=ZZ9000`, `FAKENATIVEMODES=No`, and `DISPLAYCHAIN=Yes`, then installs the selected profile as `Devs:Picasso96Settings`. |
| Poseidon USB | `Devs/USBHardware/zzusbhw.device` | Optional. Installs to `Devs:USBHardware/`; if that drawer is absent, offers to create it. |
| Network | `Devs/Networks/ZZ9000Net.device`; `Devs/NetInterfaces/ZZ9000Net` | Saves the entered MAC address in `ENVARC:ZZ9K_MAC`, installs the SANA-II driver in `Devs:Networks/`, and installs the DHCP Roadshow template in `Devs:NetInterfaces/`. |
| AHI and MHI audio | `Devs/AHI/zz9000ax.audio`; `Devs/AudioModes/ZZ9000AX`; `Libs/MHI/mhizz9000.library` | Installs the AHI subdriver in `Devs:AHI/`, its AudioMode in `Devs:AudioModes/`, and the MHI library in `LIBS:MHI/`. |
| SDK runtime | `Libs/zz9k.library`; `Libs/mpega.library`; `Classes/DataTypes/zz9k-picture.datatype`; `Storage/DataTypes/`; `Tools/zz9k-#?`; `Tools/ZZPlay#?` | Offered only when `zz9k.library` is in the release drawer. Installs `zz9k.library` and optional `mpega.library` in `LIBS:`, the datatype class in `SYS:Classes/DataTypes/`, JPEG/PNG descriptors in inactive `SYS:Storage/DataTypes/`, the `zz9k-*` commands in `C:`, and `ZZPlay` in the program drawer. |
| Accelerated AmiSSL | `Libs/AmiSSL/68020-40/amissl_v362.library`; `Libs/AmiSSL/68060/amissl_v362.library` | Offered only when a staged build and `LIBS:AmiSSL/amissl_v362.library` already exist. Backs up the installed library once, then installs the chosen CPU build in `LIBS:AmiSSL/`. |
| Control panel | `Tools/ZZTop` | Installs in the program drawer. |
| Command-line tools | `Tools/ZZScanlines`; `Tools/ZZFwUpdate`; `Tools/ZZNetStats`; `Tools/ZZDiag`; `Tools/ZZNetReady` | Installs each command in `C:`. |
| Native capture diagnostic | `Tools/ZZCapture` | Optional. Installs in `C:` when selected. |

When `ZZTop` or `ZZPlay` is installed, the final prompt defaults to adding the
program drawer to `S:User-Startup`. It writes a replaceable `;BEGIN ZZ9000` /
`;END ZZ9000` block containing `Path <drawer> ADD`, so new Shells opened after
the next reboot can find those programs by name.

## Manuals

The script installs `ZZTop.guide` and `ZZPlay.guide` beside their programs. It
puts the other manuals in the selected manuals drawer when their component is
installed.

| Manual | Installed with |
| --- | --- |
| `ZZ9000Drivers_Installer.guide` | Always |
| `usb-poseidon.guide` | `zzusbhw.device` |
| `ahi.guide` | AHI and MHI audio drivers |
| `sdk.guide` | SDK runtime |
| `amissl.guide` | Accelerated AmiSSL |
| `tools.guide` | Command-line tools |
| `ZZCapture.guide` | `ZZCapture` |

## Configuration, upgrades, and troubleshooting

### Picasso96

`ZZ9000.card` 2.4 and later uses the `BT_MNT_ZZ9000` board type. On the first
upgrade of an existing, unmarked `Devs:Picasso96Settings`, the script creates
`Devs:Picasso96Settings.pre-ZZ9000-2.4` and, if absent,
`Devs:Picasso96Settings.bak` before replacing the settings file. It records the
installation in `ENVARC:ZZ9000.P96Settings`, so later runs skip that migration.
Keep a separate copy of custom settings before selecting either supplied
profile.

`Picasso96Settings` is the shared Zorro II and Zorro III profile.
`Picasso96Settings-Z3` is for Zorro III only. Both contain the monitor DPMS
Standby, Suspend, and Active Off capabilities used by `ZZ9000.card`. The stock
templates use a 148571429 Hz pixel clock for 1920x1080. See the
[output and refresh profiles](../README.md#scandoubler-output-and-refresh) for
firmware and card requirements.

Native chipset video normally starts with `filtered_60`: 800x600 for PAL input
and 720x480 for NTSC input, both at nominal 60 Hz. Use ZZTop's **Display** page
to select **Output** and **Refresh**, then save the setting. Enable
**Settings → Advanced controls** to access the **Capture** page. Remove legacy
`ENVARC:ZZ9000-VCAP-800x600`, `ENVARC:ZZ9000-NS-VSYNC`, and
`ENVARC:ZZ9000-NS-VSYNC-NTSC` overrides when ZZTop manages native output.

### Poseidon USB

Install Poseidon before selecting `zzusbhw.device`. If Poseidon does not bind
the device after installation, open Trident's Hardware page, add
`zzusbhw.device` with unit `0`, save, then reboot or restart Poseidon. Read
`usb-poseidon.guide` for setup and fault diagnosis.

### Network and audio

The installer writes its default MAC address prompt to `ENVARC:ZZ9K_MAC`. To
change it later, edit that file. Firmware 2.3 and later can also read `mac`
from `ZZ9000.CFG`; the ENV value takes precedence.

Use ZZTop's Audio page for normal audio settings. `ZZ9K_MIX_LEVELS` and
`ZZ9000AX-NOLPF` are no longer used. `ENV:ZZ9K_INT2` remains available for
interrupt-conflict troubleshooting and affects both audio drivers.

### SDK runtime and AmiSSL

The SDK runtime needs SDK-service firmware. On matched Zorro II firmware,
bitstream, SDK payload, and driver sets, SDK clients share one host window:
16 KiB on current generation-2 layouts, which reserve the other 48 KiB for the
audio direct ring, and 64 KiB on generation 1. The 4 MiB profile supports one
bounded 224 KiB `ZZPlay` PIP source; the 2 MiB profile has no video PIP. Concurrent accelerated operations can
contend for that window. Legacy descriptor-free 4 MiB compatibility keeps the
fixed 64 KiB window without negotiated layout or PIP; legacy 2 MiB and unknown
sizes reject it.

The accelerated AmiSSL option replaces
`LIBS:AmiSSL/amissl_v362.library` only after preserving it as
`amissl_v362.library.bak`, unless that backup already exists. Choose
`68020/030/040/080 or MorphOS` for 68020, 68030, 68040, or Apollo 68080, and
choose `68060 or Amithlon` for 68060 or Amithlon. To revert, delete the
accelerated library and rename the `.bak` file. Supported TLS work uses the
ZZ9000 path; unsupported algorithms use AmiSSL's software provider.

### ZZCapture

`ZZCapture info` and `ZZCapture observe` collect native-video timing evidence
without changing capture phase. `phase`, `check`, `startup`, and `calibrate`
are experimental A4000/C28 diagnostics. Use them only with the matching
firmware and bitstream, run `Stack 32768` first, and read `ZZCapture.guide`.
The tool does not save `ZZ9000.CFG`.

## Release contents and local packaging

The release archive contains `README.md`, `ZZ9000Installer.info`, and the
`ZZ9000Installer/` drawer at its root. Driver and tool binaries are contained
inside that drawer, not duplicated at the archive root. Release packaging also
converts text manuals and the installer script from repository UTF-8 to
Amiga-compatible ISO-8859-1.

The repository keeps icons and configuration templates. CI or local packaging
supplies binary payloads, generated manuals, SDK runtime files, and the
per-CPU AmiSSL builds. A fresh clone is therefore not an installable release.
For a local package with the same staging layout, build the required components
and run:

```sh
make package-local
```

This runs `tools/package-local.sh`, which stages the package as
`zz9000-drivers-<tag>/` and generates `Docs/ZZ9000Drivers_Installer.guide` from
this file. The CI release job performs the equivalent packaging for `v*` tags.

`zzsd.device` is not installed by this script. It is packed into firmware
`BOOT.bin`; see [sd-boot/README.md](../sd-boot/README.md).

## Packaging history

| Change | Current effect |
| --- | --- |
| `ZZ9000.card` 2.4 adopted `BT_MNT_ZZ9000`. | Existing P96 settings can require migration, so the installer preserves the two backups described above. |
| Poseidon superseded `ZZ9000USBStorage.device`. | The installer offers to remove `Devs:ZZ9000USBStorage.device`. |
| `mhizz9000.library` moved to `LIBS:MHI/`. | The installer offers to remove an obsolete `LIBS:mhizz9000.library`. |
| ZZTop moved from `SYS:Tools/` to the `ZZ9000` program drawer. | The installer offers to remove `SYS:Tools/ZZTop` after installing the current copy. |

## Updating the installer

When adding an artifact, update both the installer script and the package
staging source:

1. Add the appropriate `(copyfiles ...)` step to
   `installer/ZZ9000Installer/Install ZZ9000`, with the release-drawer
   `(source ...)` and AmigaOS `(dest ...)`.
2. Add the matching staging step to `tools/package-local.sh` and the `release`
   job in `.github/workflows/ci.yml`.
