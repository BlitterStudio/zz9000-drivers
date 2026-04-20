# ZZ9000 Drivers (AmigaOS) — BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 AmigaOS driver sources. It is
> maintained by Dimitris Panokostas / **BlitterStudio** and is **not
> affiliated with, endorsed by, or supported by MNT Research GmbH**.
> The ZZ9000 hardware itself is designed and manufactured by MNT
> Research GmbH — hardware questions belong with them; driver issues
> and fork-specific discussion belong here.
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-drivers

## Graphics

The graphics driver is in the "rtg" folder. It is compatible to P96 (formerly Picasso96) but does not use any dependencies derived from those projects.

### Build

The graphics drivers are built with the vbcc compiler and m68k-amigaos target: http://sun.hasenbraten.de/vbcc/
Execute build.sh to create ZZ9000.card. ZZ9000.card goes into Libs:Picasso96 on the target device.

Alternatively, use the Docker-based cross-compiler:

```bash
docker run --rm -v $(pwd):/src -w /src/rtg \
  sacredbanana/amiga-compiler:m68k-amigaos \
  sh -c "export PATH=/opt/amiga/bin:\$PATH && sh build.sh"
```

### ZZTop (Configuration GUI)

```bash
docker run --rm -v $(pwd):/src -w /src/ZZTop \
  sacredbanana/amiga-compiler:m68k-amigaos \
  m68k-amigaos-gcc Sources/ZZTop.c -m68030 -O2 -o ZZTop -Wall -Wextra \
  -Wno-unused-parameter -lamiga -noixemul -lm
```

### ZZScanlines (Scanline Control CLI)

```bash
docker run --rm -v $(pwd):/src -w /src/ZZScanlines \
  sacredbanana/amiga-compiler:m68k-amigaos \
  m68k-amigaos-gcc -O2 -noixemul -o ZZScanlines ZZScanlines.c -lamiga
```

### CI

Both repos have `.gitlab-ci.yml` pipelines that build on every push.

## Network

The network drivers reside in the "net" folder. The driver is SANA-II compatible. Execute build.sh to create ZZ9000Net.device. ZZ9000Net.device goes into Devs:Networks on the target device.

# License / Copyright

Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
MNT Research GmbH, Berlin
https://mntre.com

Network driver based on work by
(C) 2018 Henryk Richter <henryk.richter@gmx.net>

Scanline control tools (ZZScanlines CLI, ZZTop slider) pair with
scanline bitstream V1 and V2 by Xanxi. V2 adds
multi-mode patterns (classic / soft / gradient) with odd/even parity,
gated to AGA scandoubled modes and RTG resolutions below 350 lines.
ZZScanlines.c V2 is essentially Xanxi's reference implementation,
adapted for the drivers repo conventions.

RTG driver optimizations (SetColorArray Z3 batch path, AllocBitMap)
and scanline tooling (ZZScanlines V2 port, ZZTop V2 slider retargeting
with hardware readback) by Dimitris Panokostas (midwan).

USB Poseidon hardware driver (zzusbhw.device) — chunked bulk transfers,
root-hub emulation, async INT via poll task, Z3 autoconfig preference,
CopyMemQuick fast path, mailbox protocol, throughput optimisation —
by Dimitris Panokostas (midwan). Paired with the ZZ9000OS firmware
USB stack in the companion repository.

SPDX-License-Identifier: GPL-3.0-or-later
https://spdx.org/licenses/GPL-3.0-or-later.html
