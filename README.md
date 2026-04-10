# MNT ZZ9000 Drivers (AmigaOS)

More Info: https://mntre.com/zz9000

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

Copyright (C) 2016-2019, Lukas F. Hartmann <lukas@mntre.com>
MNT Research GmbH, Berlin
https://mntre.com

Network driver based on work by
(C) 2018 Henryk Richter <henryk.richter@gmx.net>

Scanline support tools (ZZScanlines, ZZTop slider) based on original
scanline bitstream by Xanxi, adapted for firmware 1.13+.

RTG driver optimizations (SetColorArray Z3 batch path, AllocBitMap)
and scanline tooling by Dimitris Panokostas (midwan).

SPDX-License-Identifier: GPL-3.0-or-later
https://spdx.org/licenses/GPL-3.0-or-later.html
