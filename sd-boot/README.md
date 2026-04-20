<!--
  Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
  SPDX-License-Identifier: GPL-3.0-or-later
-->

# zzsd.device — SD-card-backed boot driver for ZZ9000

`zzsd.device` lets the ZZ9000 boot AmigaOS from a **hardfile** (`.hdf`)
stored as a regular file on a FAT-formatted SD card. The whole stack
is self-contained: the firmware reads the HDF via FatFs, exposes its
contents through a shared Zorro window, and a small AmigaOS device
driver shipped inside the board's autoboot ROM presents the HDF's
RDB-partitioned contents as bootable volumes.

## What you need

* An SD card formatted as **FAT32** (not ExFAT).
* A file named exactly `/zz9000.hdf` at the root of that card.
* The HDF must use **Amiga RDB partitioning** — the first 16 blocks
  must contain an `RDSK` block pointing at one or more `PART` blocks
  plus `FSHD`/`LSEG` entries for any custom filesystems (PFS3, SFS,
  etc.). UAE "single-partition hardfiles" (those starting with
  `DOS\N`/`PFS\N`/`SFS\N` at block 0) are **not** supported — you have
  to repartition them with HDToolBox or create a new RDB image in
  WinUAE/FS-UAE/Amiberry.
* Plus the usual `BOOT.bin` on the SD card for ZZ9000 itself.

No HDF? Boot still works; the driver just never publishes a volume,
and other boot sources (DF0:, ROM) take over normally.

## How it boots

1. Zorro autoconfig hands Kickstart the ZZ9000's DiagArea (8 KB at
   `cardbase+0x6000`). The first 32 bytes are the standard Amiga
   DiagArea header; then a small m68k thunk (the "diag code", built
   from `boot-rom/boot.S`); then the relocatable `zzsd.device` image
   packed in after a fixed offset so everything fits inside the FPGA-
   decoded 8 KB ROM window.
2. Kickstart calls the DiagEntry thunk, which:
   - reads `zzsd.device` byte-by-byte from the Zorro ROM window into a
     private buffer (sidestepping Kickstart's 8 KB DiagArea copy),
   - relocates the HUNK_HEADER/CODE/RELOC32/END stream into freshly
     allocated Amiga RAM,
   - patches a RomTagCopy in the DiagArea so Kickstart's resident
     scan finds the driver and AutoInit-creates `zzsd.device`.
3. `init_device()` opens `expansion.library`, locates the ZZ9000's
   `ConfigDev`, issues a **GETINFO** to the firmware, snapshots the
   returned RDB metadata into local RAM, then streams each partition's
   filesystem LSEG binary in 16 KB chunks through the shared buffer
   (`LOADFS` command, one chunk per call), relocates it with a
   lightweight HUNK parser, registers the `FileSysEntry` in the system
   `FileSystem.resource`, builds a `DeviceNode`, and calls
   `AddBootNode()` for every bootable partition.

## Shared-buffer protocol

All communication with the firmware goes through register writes at
`cardbase+0xC0..0xCF` and a 24 KB shared buffer at
`cardbase+0xA000..0x10000` that maps to `USB_BLOCK_STORAGE_ADDRESS` in
the firmware.

| Register       | Offset | Direction | Purpose |
|----------------|--------|-----------|---------|
| `BOOT_CMD`     | 0xC2   | write     | Packed 16-bit cmd: bits [3:0]=cmd, [15:4]=chunk index. cmd 1 = GETINFO, 2..9 = LOADFS for filesystem 0..7 |
| `BOOT_STATUS`  | 0xC4   | read      | Non-0xFFFF = cmd done; 0 = OK, anything else = error |
| `SDBLK_TX_HI`  | 0xB4   | write     | Upper 16 bits of block number for WRITE |
| `SDBLK_TX_LO`  | 0xB6   | write     | Lower 16 bits, triggers the write |
| `SDBLK_RX_HI`  | 0xB8   | write     | Upper 16 bits of block number for READ |
| `SDBLK_RX_LO`  | 0xBA   | write     | Lower 16 bits, triggers the read |
| `SD_STATUS`    | 0xBC   | read/write| Sets `num_blocks` on write; returns firmware status on read (`0xFFFF` = busy, 0 = OK, else error) |

Block transfers move 1–48 blocks (max 24 KB = the visible buffer
window) per round-trip. The driver loops internally for larger I/O.

## Known limitations

* **Polling-only completion.** The driver spins on `SD_STATUS` until
  the firmware clears the BUSY sentinel. There's no interrupt path, so
  the m68k CPU is fully occupied during each I/O. Observed effects on
  heavy workloads (SysSpeed, large copies):
  - bulk throughput plateaus around 1–2 MB/s even though raw bus
    bandwidth is higher — most of the wall-clock time is polling,
  - mouse and other UI elements stutter while sustained I/O runs.

  The planned fix is an FPGA-assisted Zorro interrupt (see
  `docs/FPGA_PLAN.md`). Until then, single-threaded heavy I/O is going
  to feel sluggish.

* **HDF is opened once at firmware boot.** If the card is removed
  afterwards, writes fail mid-stream and subsequent I/O returns errors;
  there is no hot-remount. Power-cycle to recover.

* **RDB-only HDFs.** No DOS-style single-partition hardfiles.

* **PFS3 must match the on-disk magic**. The `FSHD` DosType in the RDB
  has to match the magic byte in the filesystem's root block; a
  mismatch (e.g. RDB says `PFS\0` but the volume was formatted as
  `PFS\3`) causes `pfs3aio` to fail with "Initialization Failure".

## Layout & files

```
sd-boot/
├── README.md            ← this document
├── build.sh             ← Docker + m68k-amigaos-gcc, DEBUG=1 to enable trace
├── zzsd_device.c        ← Exec device exec-vectors (open/close/beginio/...)
├── zzsd_cmd.c           ← low-level block-read/write Zorro protocol
├── zzsd_boot.c          ← GETINFO + LOADFS handling, FSHD relocation, AddBootNode
├── zzsd_cmd.h           ← struct SDBase, register map, protocol constants
└── boot-rom/
    ├── boot.S           ← m68k DiagEntry/BootEntry + hunk relocator
    ├── boot.bin         ← assembled artifact (tracked for CI)
    └── diag-code.h      ← hex dump of boot.bin, embedded in firmware
```

`zzsd.device` and `zzsd-device.h` are build artifacts and are in
`.gitignore`. After building here, copy `zzsd-device.h` into the
firmware tree (`ZZ9000OS/src/zzsd-device.h`) and rebuild the firmware
so the updated driver lands in `BOOT.bin`.

## Building

```sh
# Normal build
bash build.sh

# With full debug trace to the Zorro serial console
DEBUG=1 bash build.sh
```

The diag-area thunk is assembled separately (from `boot-rom/boot.S`).
If you modify it, rebuild with the firmware's convenience script or
manually with `m68k-amigaos-gcc -x assembler-with-cpp -c -m68020`.
