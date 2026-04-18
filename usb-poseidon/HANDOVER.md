# ZZ9000 Poseidon USB Stack — Handover Notes

Current state as of **driver v1.88** (`91a2ad0`) + **firmware commit
`dd813e6`**. Both committed snapshots; roll back to them if a later
change regresses. Prior stable baseline was driver v1.83 (`7c84b5b`) +
firmware `69008e3`.

---

## What's working

| Scenario | Status | Notes |
|---|---|---|
| Bring online / Trident open / empty port | ✅ stable | No crash, no hang |
| LS wired mouse (HID) | ✅ works | Smooth, no cursor slide |
| HS wireless mouse receiver (HID) | ✅ works | Slight lag vs native — see "HS polling tradeoff" |
| Keyboard (HID) | ✅ works | |
| Mass storage mount (FAT32 / FFS RDB) | ✅ works | SanDisk HS stick, etc |
| Large file copy (8 MB+ tested) | ✅ works | ~4.4 MB/s write, ~3.2 MB/s read (v1.88) |
| USB audio class | ✅ works | Verified earlier |
| USB Ethernet (ASIX AX88772) | ✅ works | Clean disconnect, no crash |
| Hot-plug detect / re-enumerate | ✅ works | |
| Poseidon Info window (version, mfr, product, etc.) | ⚠️ partial | QUERYDEVICE writes correct values but Poseidon doesn't display them — suspect Poseidon reads from its own cache populated at bind time |

## Known gaps vs Deneb

Bulk throughput benchmark (SanDisk 2.0 HS, FAT32, A4000 Z3):

```
                 v1.83       v1.88       Deneb
Create           365         349         385       (ops/s)
Open             537         487         544
DirScan         11330       10798        9575      ← we still beat Deneb
Delete           3920         546         587      ← 3920 was a cached run
Seek/Read          —         3572          —
CreateFile      3.10        4.02         5.63      (MB/s)
WriteFile       3.20        4.37         6.34      (+37% over v1.83)
ReadFile        2.51        3.21         5.91      (+28% over v1.83)
```

Metadata ops: at parity.
Bulk throughput: was ~50% of Deneb in v1.83, now ~69% of Deneb in
v1.88. Remaining gap is bounded by the EHCI QTD page limit (5 × 4 KB
= 20 KB per single QTD) and by AXI contention on 2-QTD chains — see
"Remaining optimisation levers" below.

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
 │ - chunked bulk (16 KB)   │
 │ - async int via poll task│
 │ - prefers Z3 autoconfig  │
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

## Optimisation levers — status

### ✅ Done in v1.88

#### 1. `CopyMemQuick` in `safe_copy` fast path — *landed, marginal*
- Long-aligned fast path now calls exec's `movem.l`-based bulk copy
  instead of the C long-per-move loop. Word and byte tails unchanged.
- **Measured gain**: within benchmark noise. The bottleneck is
  Zorro-bus + AXI, not CPU instruction fetch, so the movem amortisation
  didn't help. Kept anyway — architecturally cleaner and uses the
  exec primitive the rest of the codebase prefers.

#### 1b. Zorro III autoconfig preference — *landed, no perf impact*
- `init_device` now tries product `0x6d6e:0x4` (Z3) first and falls
  back to `0x6d6e:0x3` (Z2). Matches the convention used by RTG, net,
  AHI, MHI, ax-direct.
- **Measured gain**: none on an A4000. The driver does bind at the Z3
  BAR (confirmed), but throughput stayed flat — the FPGA Zorro state
  machine has many Z3 substates (`Z3_WRITE_UPP/LOW/PRE/FIN/DTACK/…`)
  whose per-cycle latency appears to eat the "4 bytes per cycle vs 2"
  width advantage. Kept anyway — no downside, and Z3-slot Amigas now
  use the Z3 BAR like every other driver.

#### 2. Firmware dma_buf elimination — *landed, marginal standalone*
- `handle_bulk_xfer` DMAs directly from/to the shared mailbox
  (`USB_BLOCK_STORAGE_ADDRESS + ZZUSB_DATA_OFFSET`, 64-byte aligned).
  Saves one 8–16 KB DDR→DDR memcpy + one cache op per chunk.
- **Measured gain**: ~2–3% standalone. Real value was freeing AXI
  headroom so driver-side `BULK_CHUNK` could rise from 8 KB to 16 KB.
- `handle_ctrl_xfer` and `handle_int_xfer` still use `dma_buf` —
  bulk is the only hot path where this matters.

#### 2b. BULK_CHUNK raised 8 KB → 16 KB — *landed, big win*
- Per-chunk fixed overhead (cache ops, EHCI queue setup, mailbox
  round-trip) was the dominant bottleneck once dma_buf was gone.
  Raising BULK_CHUNK halves that overhead per request.
- **Measured gain**: **+37% write, +28% read** vs v1.83.
- 16 KB is the ceiling for a single EHCI QTD (5 × 4 KB pages = 20 KB
  max). 24 KB forces a 2-QTD chain whose second QTD starves on AXI
  contention and returns status=0x20, wedging the Amiga.

### 🔲 Remaining

Ordered from cheapest to most invasive:

#### 3. 20 KB BULK_CHUNK sweet-spot test — *quick, low risk*
- 16 KB leaves ~4 KB of single-QTD headroom on the table. 20 KB is
  the exact EHCI single-QTD maximum (5 × 4 KB pages, no QTD chain).
  Worth a single-line experiment before going to bigger levers.
- **Cost**: driver rebuild only.
- **Risk**: low. If it regresses due to fewer spare pages, revert
  to 16 KB.

#### 4. Vivado AXI QoS tuning
- The 24 KB failure is specifically the **second QTD** starving on
  AXI contention. Raising EHCI's AXI priority above the RTG engine
  should let 2-QTD chains (and potentially 32 KB / 64 KB chunks via
  a larger buffer) run cleanly.
- **Cost**: requires Vivado synthesis + bitstream + coordinated
  release.
- **Risk**: medium — QoS changes affect graphics latency too.

#### 5. Shared-buffer expansion (24 KB → 64 KB+)
- Only useful **in combination with** pipelining or a multi-QTD
  chunk path; a single 64 KB EHCI transfer would need 4 linked QTDs
  and hit the same AXI wall as 24 KB does today unless #4 lands
  first. Otherwise just exposes a bigger buffer that the driver
  can't push through faster.
- **Cost**: firmware memory-map change (move `MNT_FB_BASE` from
  `0x10000` to `0x20000`, expand USB block to `0xA000–0x1FFFF`).
  **Breaking change** for any RTG / P96 setup that hard-codes
  `0x10000` as framebuffer base. Needs coordinated driver +
  firmware + RTG release.
- **Risk**: high without RTG coordination.

#### 6. Mailbox command pipelining
- Overlap driver copy with firmware EHCI so the two sides aren't
  serially waiting on each other.
- **Cost**: firmware adds a command ring; driver re-architects bulk
  loop. Protocol redesign.
- **Risk**: high. Probably only worth doing after #4 and #5.

---

## Investigation pitfalls encountered (don't repeat)

- **Task-struct setup in `AddTask`**: the NDK's LP3 macro uses an `"rf"` gcc constraint that lets the compiler place `initPC`/`finalPC` in d0/d1 instead of a2/a3. Symptom: task dispatches with PC=0, crashes with "Word read from 0" per MuForce. Fixed in `zzusbhw_device.c` with hand-rolled inline asm around the `AddTask` jsr. **Do not replace that inline asm with the macro.**

- **ULPI FS4LS default**: earlier firmware hard-coded ULPI PHY to FS4LS as a workaround for LS-device reset wedging. This capped all HS devices to 12 Mbit/s. Fixed with dynamic XCVR switching — HS by default, FS4LS only for LS devices detected by line state.

- **Empty-port bring-online crash**: the async poll task was being created on every `begin_io` (even for QUERYDEVICE/USBRESET), and the first dispatch of that task crashed due to the `AddTask` bug. Now created lazily on first downstream INTXFER only, so empty-port scenarios never materialise the task.

- **Bulk chunk size vs EHCI QTD page limit**: EHCI QTDs carry 5 × 4 KB
  buffer pages = 20 KB max per single QTD. 16 KB stays comfortably
  within that and is the current stable setting (v1.88). 24 KB forces
  a 2-QTD chain; the second QTD starves on AXI contention and fails
  with status=0x20 (EHCI Data Buffer Error), wedging the Amiga via
  Poseidon's recovery path. The earlier v1.82 regression at 16 KB was
  driven by the firmware-side `memcpy(dma_buf, …)` loading AXI on top
  of EHCI DMA — removing that bounce copy (v1.88 firmware) is what
  made 16 KB stable. Raising further (20 KB single-QTD test, or 24 KB+
  with multi-QTD chains) requires lever #4 (AXI QoS) unless the
  single-QTD-at-20-KB path happens to work.

- **"CPU-side copy loop" myth**: before v1.88 the HANDOVER assumed
  `safe_copy`'s C long-per-move loop was a major bottleneck
  (~300–900 µs per 8 KB chunk). Empirically, `CopyMemQuick` had **no
  measurable effect** — the mailbox is Zorro-mapped DDR on the card,
  so each long-word write is a bus cycle, not a CPU-bound memory op.
  The ~10× instruction-fetch amortisation of `movem.l` is invisible
  when the bus dominates. Don't assume CPU-side tweaks will help
  mailbox bandwidth.

- **Zorro III bus alone doesn't help**: binding to the Z3 autoconfig
  BAR (instead of Z2) didn't move throughput on an A4000. The FPGA
  Z3 state machine has many substates (`Z3_WRITE_UPP/LOW/PRE/FIN/
  DTACK/…`) whose cumulative latency offsets the nominal 2× bus-width
  advantage. We still prefer Z3 because that's what every other
  driver does and there's no downside — but don't expect Z3 alone to
  deliver a Deneb-like jump. The bottleneck is AXI-side, not bus-side.

- **Bulk `UHIOERR_HOSTERROR` / `UHIOERR_OVERFLOW`**: Poseidon's massstorage.class has fragile recovery state machines that crash on these codes under sustained errors. Map everything non-`OFFLINE` to `UHIOERR_TIMEOUT` (class drivers treat as "not ready, retry") with escalation to `USBOFFLINE` after 16 consecutive failures.

- **0-byte bulk IN success**: never reply with `io_Error=0, actual=0` on an INT xfer — Poseidon's HID class treats this as "state unchanged" and re-applies the last report, causing cursor slide. Use `UHIOERR_TIMEOUT` for NAK / no-data-yet. Bulk short packets on IN (actual < requested) ARE legit end-of-transfer markers and should return success.

- **Poseidon Info window**: populating `UHA_*` tags via direct-slot `ti_Data` writes + reporting `iouh_Actual = answered` is correct per the autodoc, but Poseidon still shows empty values. Suspect it caches info elsewhere (lib_IdString parse? psdAddHardware path?). Not a driver bug we can easily fix; treat as cosmetic.

---

## Next session picks up here

1. **Start by reading this doc + the two recent commit messages**
   (`91a2ad0` drivers, `dd813e6` firmware).
2. Quickest next experiment is **lever #3** — try `BULK_CHUNK = 20480`
   (exactly the EHCI single-QTD max, one-line driver change). If stable
   it's a small additional bump over 16 KB. If it regresses, keep 16 KB
   and move to lever #4.
3. The next real throughput jump requires **lever #4 (Vivado AXI QoS)**
   so that the second QTD in a 24 KB / 32 KB / 64 KB chain doesn't
   starve. Only after that does **lever #5** (bigger shared buffer)
   become useful, and **lever #6** (pipelining) last.
4. Bus- and CPU-side tweaks are exhausted — see the two new pitfalls
   under "Investigation pitfalls encountered" for why. Don't relitigate
   `CopyMemQuick` / Z3 preference / cache ops chasing throughput.

## Benchmark methodology

User has been running Amiga-side file I/O benchmarks producing ops/s
and MB/s numbers for:

- Create / Open / DirScan / Delete / Seek+Read (ops/s)
- CreateFile / WriteFile / ReadFile (MB/s)

Compared directly against Deneb using the same USB stick. Any
optimisation should be re-benchmarked the same way after each change
so regressions are caught immediately (as happened with 16 KB chunks
in v1.82).
