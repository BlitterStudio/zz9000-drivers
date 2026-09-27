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

```text
ZZFwUpdate RAM:BOOT.bin
```

Power-cycle the Amiga after replacing `BOOT.bin` so the ZZ9000 boots the
new firmware. The ZZTop Project menu offers the same operation with a
file requester.

By default the destination filename on the SD card is the source
basename. To write a different root-level filename, pass it as the
optional second argument:

```text
ZZFwUpdate SYS:Storage/zz9000-fw.bin BOOT.bin
```

The destination name must be 1-64 characters from `A-Z`, `a-z`, `0-9`,
`.`, `_`, or `-`.

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
