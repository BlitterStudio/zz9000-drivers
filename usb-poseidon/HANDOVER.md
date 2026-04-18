# ZZ9000 Poseidon USB Stack — Handover Notes

Current state as of **driver v1.83** + **firmware commit `69008e3`**.
Both committed snapshots; roll back to them if a later change regresses.

---

## What's working

| Scenario | Status | Notes |
|---|---|---|
| Bring online / Trident open / empty port | ✅ stable | No crash, no hang |
| LS wired mouse (HID) | ✅ works | Smooth, no cursor slide |
| HS wireless mouse receiver (HID) | ✅ works | Slight lag vs native — see "HS polling tradeoff" |
| Keyboard (HID) | ✅ works | |
| Mass storage mount (FAT32 / FFS RDB) | ✅ works | SanDisk HS stick, etc |
| Large file copy (8 MB+ tested) | ✅ works | ~3.2 MB/s write, ~2.5 MB/s read |
| USB audio class | ✅ works | Verified earlier |
| USB Ethernet (ASIX AX88772) | ✅ works | Clean disconnect, no crash |
| Hot-plug detect / re-enumerate | ✅ works | |
| Poseidon Info window (version, mfr, product, etc.) | ⚠️ partial | QUERYDEVICE writes correct values but Poseidon doesn't display them — suspect Poseidon reads from its own cache populated at bind time |

## Known gaps vs Deneb

Bulk throughput benchmark (SanDisk 2.0 HS, FAT32):

```
                 ours        Deneb
Create           365         385       (ops/s)
Open             537         544
DirScan         11330        9575     ← we beat Deneb
Delete           3920         587     ← cached
CreateFile      3.10        5.63      (MB/s)
WriteFile       3.20        6.34
ReadFile        2.51        5.91
```

Metadata ops: at parity.
Bulk throughput: ~50% of Deneb. Root cause is **mailbox chunking
overhead**, not anything Amiga-side — see "Remaining optimisation
levers" below.

---

## Architecture overview

```
 ┌──────────────────────────┐
 │ Poseidon USB stack (m68k)│
 │   hid.class, mass.class, │
 │   massstorage.class, …   │
 └──────────┬───────────────┘
            │ usbhardware.device API
            ▼
 ┌──────────────────────────┐
 │ zzusbhw.device (m68k)    │   ← driver in usb-poseidon/
 │ - begin_io dispatch      │
 │ - root-hub emulation     │
 │ - chunked bulk (8 KB)    │
 │ - async int via poll task│
 └──────────┬───────────────┘
            │ Zorro II mailbox @ card+0xA000 (24 KB shared buffer)
            ▼
 ┌──────────────────────────┐
 │ ZZ9000OS (ARM / Zynq PS) │   ← firmware in ZZ9000_proto.sdk/
 │ - usb_proxy command loop │
 │ - EHCI host controller   │
 │   (USB 2.0 HS)           │
 └──────────┬───────────────┘
            │ AXI → ULPI PHY → USB port
            ▼
       USB device
```

## Key files

### Drivers repo (`~/Gitlab/zz9000-drivers/usb-poseidon/`)

- `zzusbhw.h` — structs, constants (`DEVICE_REVISION`, `ZZUSB_MAX_XFER`, etc.)
- `zzusbhw_device.c` — the whole driver: begin_io, CONTROLXFER / BULKXFER / INTXFER handlers, root-hub emulation, poll task
- `build.sh` — builds `zzusbhw.device` via Docker m68k-amigaos toolchain
- `LICENSE` — GPL-3.0-or-later

### Firmware repo (`~/Gitlab/zz9000-firmware/`)

- `ZZ9000_proto.sdk/ZZ9000OS/src/usb_proxy.c` — mailbox command loop, speed/reset handler, per-xfer dispatch to EHCI
- `ZZ9000_proto.sdk/ZZ9000OS/src/usb_proxy.h` — protocol constants (must stay in sync with driver's `zzusbhw.h`)
- `ZZ9000_proto.sdk/ZZ9000OS/src/usb/ehci-zynq.c` — Zynq-specific EHCI init (USBMODE, ULPI, BURSTSIZE, `ehci_zynq_set_phy_mode()`)
- `ZZ9000_proto.sdk/ZZ9000OS/src/usb/ehci-hcd.c` — upstream-ish EHCI HCD core (TXFIFO threshold, async-ring scrubbing)
- `ZZ9000_proto.sdk/ZZ9000OS/Makefile` — compile flags including `-DCONFIG_USB_EHCI_TXFIFO_THRESH=4`
- `bootimage_work/bootimage.bif` — bootgen input (FSBL + bitstream + ZZ9000OS.elf)

## Build & deploy

### Driver (on the Mac)

```bash
cd ~/Gitlab/zz9000-drivers/usb-poseidon
./build.sh         # Docker m68k-amigaos toolchain, writes zzusbhw.device
```

Copy `zzusbhw.device` to the Amiga's `DEVS:` directory.

### Firmware (on the Mac — no Vivado needed for ARM/C changes)

```bash
cd ~/Gitlab/zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS && make
cd ~/Gitlab/zz9000-firmware/bootimage_work && \
  ../bootgen/bootgen -image bootimage.bif -arch zynq -o BOOT.bin -w on
```

Flash `BOOT.bin` to the ZZ9000 SD card / QSPI.

Vivado synthesis is only needed when HDL (`.v` files) changes — e.g.,
for bitstream-level work like scanlines v2. ARM firmware changes never
require a Vivado rebuild.

---

## Remaining optimisation levers (for throughput parity with Deneb)

Ordered from cheapest to most invasive:

### 1. `CopyMemQuick` in `safe_copy` fast path
- **Impact**: medium-high. The C long-per-move loop is ~10× slower than exec's `movem.l`-based `CopyMemQuick`. Should save ~300–900 µs per 8 KB chunk.
- **Cost**: 5-line change in `safe_copy`, rebuild driver only. `CopyMemQuick` requires 4-byte-aligned src/dst + size multiple of 4 — fall through to the manual path for anything else.
- **Risk**: low. Pure AmigaOS-supported primitive.

### 2. Firmware dma_buf elimination
- **Impact**: medium. Firmware currently does `memcpy(dma_buf, data_buf, len)` then EHCI DMAs from `dma_buf`. If we DMA directly from `USB_BLOCK_STORAGE_ADDRESS + ZZUSB_DATA_OFFSET`, we save one memcpy + two cache ops per chunk.
- **Cost**: ~20 lines in `usb_proxy.c` (`handle_bulk_xfer`). Needs firmware rebuild.
- **Risk**: low. Shared buffer is already in DDR and DMA-reachable. Check alignment (currently offset 64, which is 32-byte aligned — EHCI-safe).

### 3. Shared-buffer expansion (24 KB → 64 KB+)
- **Impact**: large. Fewer mailbox round-trips per Poseidon request amortises the fixed overhead. Could close most of the remaining throughput gap.
- **Cost**: firmware memory-map change (move `MNT_FB_BASE` from `0x10000` to `0x20000`, expand USB block to `0xA000–0x1FFFF`). **Breaking change** for any RTG driver / P96 setup that hard-codes `0x10000` as framebuffer base. Needs coordinated driver + firmware + RTG release.
- **Risk**: high if rolled out without RTG coordination. Low if staged as a version-bumped compatibility break.

### 4. Mailbox command pipelining
- **Impact**: potentially large (overlaps driver copy with firmware EHCI).
- **Cost**: firmware adds a command ring; driver re-architects bulk loop. Complex.
- **Risk**: protocol redesign. Probably only worth doing after #3.

### 5. Vivado AXI QoS tuning
- **Impact**: unknown until measured. Could reduce EHCI DBE frequency so BULK_CHUNK can safely rise to 16 KB or 24 KB.
- **Cost**: requires Vivado synthesis + bitstream + coordinated release.
- **Risk**: medium — QoS changes affect graphics too.

---

## Investigation pitfalls encountered (don't repeat)

- **Task-struct setup in `AddTask`**: the NDK's LP3 macro uses an `"rf"` gcc constraint that lets the compiler place `initPC`/`finalPC` in d0/d1 instead of a2/a3. Symptom: task dispatches with PC=0, crashes with "Word read from 0" per MuForce. Fixed in `zzusbhw_device.c` with hand-rolled inline asm around the `AddTask` jsr. **Do not replace that inline asm with the macro.**

- **ULPI FS4LS default**: earlier firmware hard-coded ULPI PHY to FS4LS as a workaround for LS-device reset wedging. This capped all HS devices to 12 Mbit/s. Fixed with dynamic XCVR switching — HS by default, FS4LS only for LS devices detected by line state.

- **Empty-port bring-online crash**: the async poll task was being created on every `begin_io` (even for QUERYDEVICE/USBRESET), and the first dispatch of that task crashed due to the `AddTask` bug. Now created lazily on first downstream INTXFER only, so empty-port scenarios never materialise the task.

- **24 KB bulk chunks**: without the firmware Zynq fixes (SDIS / BURSTSIZE / TXFIFO), 24 KB triggers EHCI Data Buffer Errors (status=0x20). Even *with* the fixes, 16 KB chunks regress throughput because they intermittently retry on AXI contention. **Stay at 8 KB** until chunk size vs stability is re-benchmarked with Vivado AXI QoS fixes.

- **Bulk `UHIOERR_HOSTERROR` / `UHIOERR_OVERFLOW`**: Poseidon's massstorage.class has fragile recovery state machines that crash on these codes under sustained errors. Map everything non-`OFFLINE` to `UHIOERR_TIMEOUT` (class drivers treat as "not ready, retry") with escalation to `USBOFFLINE` after 16 consecutive failures.

- **0-byte bulk IN success**: never reply with `io_Error=0, actual=0` on an INT xfer — Poseidon's HID class treats this as "state unchanged" and re-applies the last report, causing cursor slide. Use `UHIOERR_TIMEOUT` for NAK / no-data-yet. Bulk short packets on IN (actual < requested) ARE legit end-of-transfer markers and should return success.

- **Poseidon Info window**: populating `UHA_*` tags via direct-slot `ti_Data` writes + reporting `iouh_Actual = answered` is correct per the autodoc, but Poseidon still shows empty values. Suspect it caches info elsewhere (lib_IdString parse? psdAddHardware path?). Not a driver bug we can easily fix; treat as cosmetic.

---

## Next session picks up here

1. **Start by reading this doc + the two recent commit messages.**
2. Next planned optimisation is `CopyMemQuick` in `safe_copy` (lever #1). Driver change only, ~5 lines.
3. After that, firmware `dma_buf` elimination (lever #2).
4. Shared-buffer expansion is staged for later — requires coordinated release and RTG compatibility planning.

## Benchmark methodology

User has been running Amiga-side file I/O benchmarks producing ops/s
and MB/s numbers for:

- Create / Open / DirScan / Delete / Seek+Read (ops/s)
- CreateFile / WriteFile / ReadFile (MB/s)

Compared directly against Deneb using the same USB stick. Any
optimisation should be re-benchmarked the same way after each change
so regressions are caught immediately (as happened with 16 KB chunks
in v1.82).
