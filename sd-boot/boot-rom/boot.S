/*
 * ZZ9000 autoboot DiagArea thunk — Kickstart entry points
 * (DiagEntry / BootEntry), a runtime hunk relocator for the packed
 * zzsd.device image, and the RomTagCopy template Kickstart's
 * resident scan picks up.
 *
 * Copyright (C) 2026, MNT Research GmbH
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

main:
RomTagCopy:
  .short 0x4afc
  .long 0
  .long 0
  .long 0
  .long 0
  .long 0
  .long 0
  .short 0

  LVOFindResident = -96
  RT_INIT = 22

BootPt:
  lea     DosName(pc),a1
  jsr     LVOFindResident(a6)
  move.l  d0,a0
  move.l  RT_INIT(A0),a0
  jsr     (a0)
  rts

DiagPt:

  movem.l d0-d7/a0-a6,-(sp)

  lea cardbase(pc), a1
  move.l a0, (a1)
  lea loadaddr(pc), a1
  move.l a2, (a1)
  lea configdev(pc), a3
  move.l a3, (a1)

  lea RomTagCopy, a5
  move.l a2, a4
  add.l #0x20, a4
  move.l a4, 2(a5)

  // Driver source: read from the ROM window via Zorro (boardaddr+0x6300).
  // Kickstart's autoconfig DiagArea copy is capped at ~8 KB regardless of
  // our da_Size, and the FPGA bitstream only maps 0x6000..0x8000 (8 KB)
  // to BOOT_ROM — 0x8000+ goes to the ethernet TX frame, returning zeros
  // for our purposes. Driver lives at ROM offset 0x300; Amiga reads at
  // cardbase + 0x6000 + 0x300 = 0x6300. z3660 does the same idea (driver
  // code stays in ROM, not in DiagArea).
  add.l #0x300, a2                 // keep a2 advanced for any later users
  move.l cardbase(pc), a0
  add.l #0x6300, a0

  // Copy the driver binary to a freshly-allocated buffer before touching
  // anything else. The driver currently lives inside the Kickstart-allocated
  // DiagArea, and the per-hunk AllocMem calls below tend to hand back memory
  // that overlaps that region — once CopyMem writes into a hunk, it corrupts
  // the source bytes we're still walking. Relocating from a private buffer
  // eliminates the overlap entirely.
  move.l a0, d2                  // d2 = driver src
  // Ask for 32 KB so Amiga is forced to find a contiguous free block
  // that's almost certainly outside the DiagArea (which is only 8 KB).
  // A small request fragmented around the DiagArea yielded a buffer
  // whose tail addresses were non-responsive.
  move.l #0x8000, d0             // 32 KB
  move.l #0x10000, d1            // MEMF_CLEAR
  move.l 4, a6
  jsr -198(a6)                   // AllocMem
  tst.l d0
  bne .have_buf
  // Fallback: try 8 KB
  move.l #0x4000, d0
  move.l #0x10000, d1
  move.l 4, a6
  jsr -198(a6)
  tst.l d0
  beq .skip_copy
.have_buf:
  move.l d0, d3                  // d3 = buffer

  // Use Exec's CopyMem (LVO -624). It handles cache coherency correctly
  // on all CPUs and processes overlap via backwards copy when dst > src.
  // CopyMem(A0=src, A1=dst, D0=size) — no return.
  // Copy 0x1D00 bytes — max we can read before running into 0x8000 from
  // driver src (0x6300). Reading past 0x8000 hits the ethernet TX frame
  // (returns zeros) and would poison the buffer tail.
  move.l d2, a0
  move.l d3, a1
  move.l #0x1D00, d0
  move.l 4, a6
  jsr -624(a6)                    // CopyMem
  move.l 4, a6
  jsr -636(a6)                    // CacheClearU

  move.l d3, a0                  // relocate from the private copy
.skip_copy:

  jsr relocatehunk
  move.l d0, d7

  // if relocatehunk returned 0, it failed
  tst.l d7
  beq diag_fail

  // copy RomTag fields
  lea RomTagCopy, a4
  move.l 14(d7), d0
  move.l d0, 6(a4)
  move.l 18(d7), d0
  move.l d0, 10(a4)
  move.l 22(d7), d0
  move.l d0, 14(a4)
  move.l 26(d7), d0
  move.l d0, 18(a4)
  move.l 30(d7), d0
  move.l d0, 22(a4)

  move.l 22(a5), d0
  jsr puthex

  lea dbgok(pc), a6
  jsr puts

diag_fail:
  movem.l (sp)+, d0-d7/a0-a6
  move.l #1, d0
  rts

.align 4
configdev:
  .long 0
cardbase:
  .long 0
loadaddr:
  .long 0
DosName:
  .string "dos.library"

.align 4
dbgok:
  .string "OK!\r\n"
dbgalloc:
  .string "ALLC:\r\n"
dbgallocfail:
  .string "ALLCFAIL\r\n"
/* PARS/UNK/ABS/END debug removed — these printed inside the hunk
 * parse loop and added ~40 bytes per site. Kept ALLC/ALLCFAIL because
 * an allocation failure during diag is the most common bug class. */

.align 4
puts:
  move.l cardbase(pc), a5
  moveq #0, d6
putsloop:
  move.b (a6)+, d6
  move.w d6, 0xf0(a5)
  bne putsloop
  rts

.align 4
puthex:
  move.l cardbase(pc), a5
  swap d0
  move.w d0, 0xf2(a5)
  swap d0
  move.w d0, 0xf2(a5)
  move.w #0x0d, 0xf0(a5)
  move.w #0x0a, 0xf0(a5)
  rts

  HUNK_HEADER = 0x03f3
  HUNK_CODE = 0x03e9
  HUNK_DATA = 0x03ea
  HUNK_BSS = 0x03eb
  HUNK_RELOC32 = 0x03ec
  HUNK_SYMBOL = 0x3f0
  HUNK_END = 0x03f2
  SEG_LEN = 0
  SEG_NEXT = 4
  SEG_START = 8
  SEG_SIZE = 8
  MEMF_CLEAR = 0x10000
  /* MEMF_REVERSE (bit 31) forces AllocMem to hand out memory from the
     top of the free list. Without this, Kickstart's DiagArea buffer
     stays in the general free pool and AllocMem happily returns blocks
     that overlap the driver source we're still relocating from. */
  MEMF_REVERSE = 0x80000000
  AllocMem = -198

relocatehunk:
	cmp.l   #HUNK_HEADER,(a0)+
	bne     Panic

	movem.l d2-d4/a2-a3,-(sp)

	lea			4(a0),a2
	move.l	(a2)+,d2
	addq.l	#8,a2

	lsl.l		#2,d2
	sub.l		d2,sp

  move.l  d2,d4

.alloc:
  move.l  -4(a2,d4.l),d0
  rol.l   #2,d0
  moveq.l #1,d1
  and.l   d0,d1
  and.w   #-4,d0
  addq.l  #SEG_SIZE,d0

  move.l  d0,d3
  or.l   #MEMF_CLEAR,d1
  move.l 4,a6
  jsr AllocMem(a6)

  move.l  d0,-(sp)
  lea     dbgalloc(pc),a6
  jsr     puts
  move.l  (sp)+,d0
  jsr     puthex

  move.l  d0,-4(sp,d4.l)
  move.l  d0,a0
  tst.l   d0
  beq     .allocfail
  move.l  d3,SEG_LEN(a0)
  cmp.l   d2,d4
  beq     .first
  move.l  (sp,d4.l),SEG_NEXT(a0)

.first:
  subq.l  #4,d4
  bgt     .alloc
  bra     .allocdone

.allocfail:
  lea     dbgallocfail(pc),a6
  jsr     puts
  add.l   d2,sp
  movem.l (sp)+, d2-d4/a2-a3
  moveq   #0,d0
  rts

.allocdone:

  add.l   d2,a2

  move.l  sp,a3
  move.l  d2,d4

.parse:
  move.w  (a2)+,d0
  add.w   (a2)+,d0

  cmp.w   #HUNK_CODE,d0
  beq     .hdata
  cmp.w   #HUNK_DATA,d0
  beq     .hdata
  cmp.w   #HUNK_BSS,d0
  beq     .hbss
  cmp.w   #HUNK_RELOC32,d0
  beq     .hreloc
  cmp.w   #HUNK_SYMBOL,d0
  beq     .hsyms
  cmp.w   #HUNK_END,d0
  beq     .hend

  moveq   #0,d0
  bra     .error

.hdata:
  move.l  (a3),a1
  add.w   #SEG_START,a1
  move.l  (a2)+,d0
  lsl.l   #2,d0
  move.l  a2,a0
  add.l   d0,a2
  bsr     CopyMem
  bra     .parse

.hbss:
  addq.l  #4,a2
  bra     .parse

.hsyms:
  move.l  (a2)+,d0
  beq     .parse
  addq.l  #1,d0
  lsl.l   #2,d0
  add.l   d0,a2
  bra     .hsyms

.hreloc:
  move.l  (a2)+,d3
  beq     .parse
  move.l  (a2)+,d0
  lsl.l   #2,d0
  move.l  (sp,d0.l),d0
  addq.l  #SEG_START,d0
  move.l  (a3),a1

.reloc:
  move.l  (a2)+,d1
  add.l   d0,SEG_START(a1,d1.l)

  subq.l  #1,d3
  bgt     .reloc
  bra     .hreloc

.hend:
  addq.l  #4,a3
  subq.l  #4,d4
  tst.l   d4
  bne     .parse

.quit:
  move.l  (sp),d0

.error:
  add.l   d2,sp

  movem.l (sp)+, d2-d4/a2-a3
  rts

Panic:
  move.l d0, 0xdff180
  subq.l #1, d0
  bra Panic

CopyMem:
  move.l  (a0)+,(a1)+
  subq.l  #4,d0
  bgt     CopyMem
  rts
