<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# ZZ9000 command-line tools

The installer puts the diagnostic and maintenance tools in `C:`, where
any shell finds them. This manual covers the firmware updater, the
networking tools, the board diagnostic, and the scanline controller;
`ZZCapture` has its own manual (`ZZCapture.guide` in this drawer), and
the GUI tools ZZTop and ZZPlay install to `SYS:Utilities/ZZ9000/`.

## Firmware updates — ZZFwUpdate

`ZZFwUpdate` copies a firmware image from AmigaOS to the ZZ9000's FAT32
microSD card without removing the card. The usual update flow:
| Tool | What it does |
| --- | --- |
| `ZZFwUpdate` | Installs a firmware release bundle or copies a local file to the card, and restores a saved backup. |
| `ZZNetStats` | Reads SANA-II and firmware receive counters from `ZZ9000Net.device`. |
| `ZZDiag` | Prints board, aperture, USB, video-capture, SD, audio, Ethernet, and scanline diagnostics. |
| `ZZNetReady` | Returns an AmigaDOS status for a boot script that waits for Ethernet negotiation. |
| `ZZScanlines` | Changes the active scandoubler scanline mode and parity. |

## ZZFwUpdate: firmware updates

`ZZFwUpdate` transfers a local file to the ZZ9000 microSD card without
removing it. Give it an unpacked firmware release drawer or its `BOOT.bin` to
install the bundle. If a sibling `ZZ9K/` drawer contains valid `NAME.ZZM`
files, it transfers those optional modules first and `BOOT.bin` last:

```text
ZZFwUpdate RAM:zz9000-firmware-v2.9.0-zorro3
ZZFwUpdate RAM:zz9000-firmware-v2.9.0-zorro3/BOOT.bin
```

Power-cycle the Amiga after replacing `BOOT.bin` so the ZZ9000 boots the
new firmware. The ZZTop Project menu offers the same operation with a
file requester.

By default the destination filename on the SD card is the source
basename. To write a different root-level filename, pass it as the
optional second argument:
`ZZ9000.CFG` from a release is never transferred. Firmware that does not
advertise `ZZ_FW_CAP_FWUP_MODULE_DIR` receives `BOOT.bin` only. Reboot, then
run the same command again to install the optional modules.

The existing single-file form derives the card filename from the final
component of the source path. Give a second argument to choose a different
filename:

```text
ZZFwUpdate SYS:Storage/zz9000-fw.bin BOOT.bin
```

The destination name must be 1-64 characters from `A-Z`, `a-z`, `0-9`,
`.`, `_`, or `-`.
`ZZFwUpdate` accepts these forms:

| Form | Result |
| --- | --- |
| `ZZFwUpdate <release-drawer-or-BOOT.bin>` | Installs sibling `ZZ9K/*.ZZM` modules first when present, then copies `BOOT.bin` to the root. |
| `ZZFwUpdate <source-file> [dest-name]` | Copies `source-file` to `0:/<dest-name>`. Without `dest-name`, uses the source basename. |
| `ZZFwUpdate RESTORE [name] [-y\|FORCE]` | Promotes the saved backup for `name` to the active file. `name` defaults to `BOOT.bin`; `-y` and `FORCE` skip confirmation. |

`dest-name` and the restore name are either an existing root filename of 1–64
characters from `A-Z`, `a-z`, `0-9`, `.`, `_`, and `-`, or a module target
exactly `ZZ9K/NAME.ZZM`, with an uppercase 1–8 character `NAME` made from
`A-Z`, `0-9`, and `_`. A leading slash, a volume prefix, other directories,
and deeper paths are rejected by the CLI.

During a transfer, it prints the card address, source and destination, then
byte and percentage progress. On success it prints the bytes written and,
when measurable, elapsed time and throughput. If a transfer fails after it
starts, the client aborts the temporary card file.

### Backups and rollback

When you replace `BOOT.bin`, the firmware keeps the previous image as
`BOOT.bak`. If a new firmware boots but misbehaves, roll back without
removing the card:

```text
ZZFwUpdate RESTORE
```

This promotes `BOOT.bak` to the active `BOOT.bin` (discarding the
replaced image, so no backup remains afterwards) after a confirmation
prompt. Pass `-y` to skip the prompt, or a name to restore something
other than `BOOT.bin`. Restore talks to the *running* firmware, so it
recovers a booting-but-misbehaving update; a card that does not boot at
all still needs the microSD removed and restored on another computer.
RESTORE needs firmware with FWUP cmd 5 support.

## Networking — ZZ9000Net.device

`ZZ9000Net.device` in `Devs:Networks/` is the SANA-II Ethernet driver.
It programs the firmware's multicast hash filter, so IPv6 and
service-discovery stacks receive their groups. It adopts the MAC address
from `ZZ9000.CFG` (`mac`), honours `int2 = on`, and works with any
SANA-II stack (Roadshow, Miami DX, Genesis).

The installer can also install a Roadshow NetInterface template
(`Devs:NetInterfaces/ZZ9000`) configured for DHCP. When the installer
asks about network setup, answering yes writes the template; Roadshow
then brings the interface up with no manual `NetConfig` editing.

### ZZNetStats

`ZZNetStats` opens `ZZ9000Net.device`, requests SANA-II global stats,
and prints the firmware RX queue, backpressure and drop counters:

```text
ZZNetStats
ZZNetStats DEVICE=Networks/ZZ9000Net.device UNIT=0
ZZNetStats Networks/ZZ9000Net.device 0
```

Run it before and after a throughput test to see whether drops happen
in the Amiga-side driver or in the firmware RX path.

## Board diagnostics — ZZDiag

`ZZDiag` dumps the useful hardware-facing diagnostics in one place:

```text
ZZDiag
ZZDiag 3 50
```

The optional arguments are sample count and AmigaDOS delay ticks between
samples. It reports the board identity, firmware version, VideoCap
state, USB, SD, AX/audio and Ethernet diagnostic registers — the
VideoCap section includes the detailed capture and genlock registers
when the running firmware exposes them. It also checks whether the
installed firmware, drivers and SDK payloads are a matched set; include
its output when reporting a problem.

### Boot-time link check — ZZNetReady

`ZZNetReady` answers one question for `S:User-Startup`: is the ZZ9000
present, and is its Ethernet port up? It needs no TCP/IP stack:

```text
ZZNetReady
ZZNetReady TIMEOUT=10 QUIET
```

Exit codes: `0` link ready (also returned on firmware that cannot
report link state, so the old always-start behaviour is kept), `5`
not ready - no card, no link within `TIMEOUT` seconds (default 5),
or unreadable arguments. Everything except OK returns WARN because
AmigaDOS shells run `S:User-Startup` with FAILAT 10 by default: a
failure code would abort the rest of the script before `If WARN`
can branch. A typical `S:User-Startup` gating:

```text
ZZNetReady QUIET
If WARN
  ; no card or no link - skip the stack this boot
Else
  Run SYS:Network/... ; or your stack's start script
EndIf
```

Requires firmware with `ETH_CONFIG_CAP_LINK_STATE` for real link
reporting (v2.8.1); the bit means negotiation completed since
power-on, not that the cable is in right now.

`ZZCapture` extends native-video diagnosis with row timing and C28
calibration evidence. Its phase-changing commands are experimental; see
`ZZCapture.guide` before using them.

## Scanlines — ZZScanlines

`ZZScanlines` controls the scanline modes of the scandoubler output:

```text
ZZScanlines 0
ZZScanlines 1 0
ZZScanlines 2 1
ZZScanlines 3 0
```

Modes are `0=off`, `1=classic`, `2=soft`, `3=gradient`; parity is
`0=odd dark`, `1=even dark`.

Like ZZTop's Scandoubler window, `ZZScanlines` changes the live FPGA
state immediately. To make scanlines survive a power cycle, save them to
`ZZ9000.CFG` — the ZZTop Scandoubler window's Save button does that.
