<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# zzsd.device: SD-card boot driver for ZZ9000

`zzsd.device` lets a ZZ9000 boot AmigaOS from an RDB-partitioned hardfile
(`.hdf`) on the card's FAT32 volume. Firmware opens the image at boot, maps
its clusters, and serves the image as a block device. The device in the
ZZ9000 autoboot ROM then registers the image's bootable partitions with
Kickstart. It is packed into firmware `BOOT.bin`; it is not installed as a
normal AmigaOS file.

## Set up an SD boot image

1. Format the microSD card as FAT32 and place the matching firmware
   `BOOT.bin` in its root.
2. Create an HDF with an Amiga RDB. Its first 16 blocks must contain an
   `RDSK` block, with `PART` blocks for its partitions. Include `FSHD` and
   `LSEG` entries for any filesystem that must be loaded from the image, such
   as PFS3 or SFS.
3. Copy the HDF to the card root. With no `hdf` entry in `ZZ9000.CFG`, name
   it `zz9000.hdf`.
4. Power-cycle the Amiga after changing the image or `ZZ9000.CFG`.

Images that begin with `DOS\N`, `PFS\N`, or `SFS\N` at block 0 are
single-partition hardfiles, not RDB images, and this driver does not boot
them. Repartition such an image with HDToolBox, or create an RDB image in
WinUAE, FS-UAE, or Amiberry.

If no selected HDF is available, the driver registers no SD volumes. Other
boot sources, including `DF0:` and ROM, remain available.

## Select the HDF in `ZZ9000.CFG`

`ZZ9000.CFG` is read from the FAT32 card root beside `BOOT.bin`. It accepts
one `key = value` assignment per line; keys and keyword values are
case-insensitive. The `hdf` key has these forms:

| Setting | Firmware action |
|---|---|
| No `hdf` entry | Opens `/zz9000.hdf`. |
| `hdf = workbench.hdf` | Opens `workbench.hdf` in the card root. The value must be a flat file name, not a path. |
| `hdf = off` | Disables SD boot, even when `/zz9000.hdf` exists. |

The configured name may contain up to 63 printable characters. It cannot
start with `.` or contain `/`, `\\`, `:`, `#`, `;`, or whitespace. Invalid
`hdf` values are skipped. If the file contains no valid `hdf` entry, firmware
selects the default image. In ZZTop, clear the **HDF** field to save
`hdf = off`; remove the `hdf` entry to restore the default `zz9000.hdf`
selection.

## Boot path

| Stage | Action |
|---|---|
| Firmware | Opens the selected HDF, records its capacity and RDB metadata, and resolves its FAT cluster chain into physical SD extents when possible. Block I/O uses direct SD-sector reads and writes for a mapped image; firmware falls back to FatFs when it cannot map the chain. |
| DiagArea | The 8 KiB ROM window at `cardbase+0x6000` contains the Amiga DiagArea header, code from `boot-rom/boot.S`, and the packed relocatable `zzsd.device` image. |
| Kickstart | The DiagEntry code relocates the packed driver, patches its resident tag, and lets Kickstart create `zzsd.device`. |
| Driver | `init_device()` requests boot metadata with `GETINFO`, loads required filesystem segments with `LOADFS`, registers `FileSysEntry` records, and adds an `AddBootNode()` for each bootable partition. |

The firmware returns metadata for at most 16 partitions and four filesystem
headers. The driver reuses a filesystem already registered in
`FileSystem.resource`; otherwise it loads the matching HDF `FSHD`/`LSEG`
segment.

## Shared-buffer protocol

The driver and firmware use the shared buffer at
`cardbase+0xA000..0x10000`, mapped by firmware to
`USB_BLOCK_STORAGE_ADDRESS`, plus the following registers.

| Register | Offset | Direction | Purpose |
|---|---:|---|---|
| `BOOT_CMD` | `0xC2` | Write | Packed 16-bit command. Bits 3:0 select `GETINFO` (`1`) or `LOADFS` for filesystem slots 0–7 (`2`–`9`); bits 15:4 are the `LOADFS` chunk index. The current firmware exports at most four filesystem slots. |
| `BOOT_STATUS` | `0xC4` | Read | `0xFFFF` while a command runs, `0` on success, `0xFFFE` when no HDF is configured or open, and another value on error. |
| `SDBLK_TX_HI` | `0xB4` | Write | Upper 16 bits of the block number for a write. |
| `SDBLK_TX_LO` | `0xB6` | Write | Lower 16 bits of the block number; writing it starts the transfer. |
| `SDBLK_RX_HI` | `0xB8` | Write | Upper 16 bits of the block number for a read. |
| `SDBLK_RX_LO` | `0xBA` | Write | Lower 16 bits of the block number; writing it starts the transfer. |
| `SD_STATUS` | `0xBC` | Read/write | Write the number of blocks for the next transfer. Read `0xFFFF` while busy, `0` on success, or a nonzero firmware error code. |


The quiet no-HDF path requires this ROM and firmware that returns `0xFFFE`.
Older firmware returns `0xFFFF` for this case, so the ROM keeps the existing
failure message for that combination.
Each transfer contains 1–48 512-byte blocks, or at most 24 KiB. The driver
splits larger I/O requests into those transfers.

## Limits and troubleshooting

The driver polls `SD_STATUS`; it has no interrupt completion path. Sustained
I/O occupies the m68k while it waits. Recorded tests put bulk throughput at
about 1–2 MB/s, and the Workbench pointer and other interface elements can
stutter during large copies or SysSpeed runs.

Firmware maps HDF extents during boot. Removing the card afterwards causes
I/O errors, and the card is not remounted while the Amiga is running.
Power-cycle after reinserting it.

If `pfs3aio` reports `Initialization Failure`, check the `FSHD` DosType in
the RDB and the filesystem root-block magic. They must match, for example
`PFS\0` and `PFS\3` are different values.

## Source layout

```
sd-boot/
├── build.sh          build script; set DEBUG=1 for serial trace output
├── zzsd_device.c     AmigaOS device vectors and I/O dispatch
├── zzsd_cmd.c        shared-buffer block-transfer protocol
├── zzsd_boot.c       boot metadata, filesystem loading, and boot nodes
├── zzsd_cmd.h        SDBase layout, register map, and protocol constants
└── boot-rom/
    ├── boot.S        DiagArea entry and packed-driver relocator
    └── diag-code.h   boot.S image embedded by firmware
```

`zzsd.device`, `zzsd-device.h`, `boot-rom/boot.o`, and
`boot-rom/boot.bin` are generated build artifacts. `zzsd-device.h` is copied
to the firmware source tree at `ZZ9000OS/src/zzsd-device.h`, where firmware
packs the driver into `BOOT.bin`.

## Build the driver

```sh
# Normal build
bash build.sh

# Include the Zorro serial trace
DEBUG=1 bash build.sh
```

The script uses `m68k-amigaos-gcc` when available. Otherwise it invokes
`../tools/amiga-docker.sh sd-boot ./build.sh`.
