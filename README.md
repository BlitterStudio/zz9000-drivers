# MNT ZZ9000 Drivers (AmigaOS)

More Info: https://mntre.com/zz9000

## Graphics

The graphics driver is in the "rtg" folder. It is compatible to P96 (formerly Picasso96) but does not use any dependencies derived from those projects.

### Build

The graphics drivers are built with the vbcc compiler and m68k-amigaos target: http://sun.hasenbraten.de/vbcc/
Execute build.sh to create ZZ9000.card. ZZ9000.card goes into Libs:Picasso96 on the target device.

## Network

The network drivers will reside in the "net" folder once the proper licenses have been sorted out for some source files. The driver is SANA-II compatible. Execute build.sh to create ZZ9000Net.device. ZZ9000Net.device goes into Devs:Networks on the target device.

# License / Copyright

Copyright (C) 2016-2019, Lukas F. Hartmann <lukas@mntre.com>
MNT Research GmbH, Berlin
https://mntre.com

SPDX-License-Identifier: GPL-3.0-or-later
https://spdx.org/licenses/GPL-3.0-or-later.html
