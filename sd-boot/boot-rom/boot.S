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

  // debug: print loadaddr
  lea dbgladr(pc), a6
  jsr puts
  move.l a2, d0
  jsr puthex

  lea RomTagCopy, a5
  move.l a2, a4
  add.l #0x20, a4
  move.l a4, 2(a5)

  add.l #0x400, a2
  move.l a2, a0

  // debug: print driver ptr
  lea dbgdrv(pc), a6
  jsr puts
  move.l a0, d0
  jsr puthex

  jsr relocatehunk

  // debug: print hunk result
  lea dbghunk(pc), a6
  jsr puts
  move.l d0, d7
  move.l d7, d0
  jsr puthex

  // if relocatehunk returned 0, it failed
  tst.l d7
  beq diag_fail

  // debug: print first 4 longs at hunk base
  lea dbgdata(pc), a6
  jsr puts
  move.l (d7), d0
  jsr puthex
  move.l 4(d7), d0
  jsr puthex
  move.l 8(d7), d0
  jsr puthex
  move.l 12(d7), d0
  jsr puthex

  // debug: print RomTag fields we're copying (14-30)
  lea dbgrtag(pc), a6
  jsr puts
  move.l 14(d7), d0
  jsr puthex
  move.l 18(d7), d0
  jsr puthex
  move.l 22(d7), d0
  jsr puthex
  move.l 26(d7), d0
  jsr puthex
  move.l 30(d7), d0
  jsr puthex

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

  // debug: print RomTagCopy contents
  lea dbgcopy(pc), a6
  jsr puts
  lea RomTagCopy, a5
  move.l (a5), d0
  jsr puthex
  move.l 2(a5), d0
  jsr puthex
  move.l 6(a5), d0
  jsr puthex
  move.l 10(a5), d0
  jsr puthex
  move.l 14(a5), d0
  jsr puthex
  move.l 18(a5), d0
  jsr puthex
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
dbgladr:
  .string "LADR=\r\n"
dbgdrv:
  .string "DRVP=\r\n"
dbghunk:
  .string "HUNK=\r\n"
dbgdata:
  .string "DATA:\r\n"
dbgrtag:
  .string "RTAG:\r\n"
dbgcopy:
  .string "COPY:\r\n"
dbgok:
  .string "OK!\r\n"

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
  add.l  #MEMF_CLEAR,d1
  move.l 4,a6
  jsr AllocMem(a6)

  move.l  d0,-4(sp,d4.l)
  move.l  d0,a0
  move.l  d3,SEG_LEN(a0)
  cmp.l   d2,d4
  beq     .first
  move.l  (sp,d4.l),SEG_NEXT(a0)

.first:
  subq.l  #4,d4
  bgt     .alloc

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
