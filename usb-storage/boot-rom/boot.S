main:
  LVOFindResident = -96
  RT_INIT = 22

BootPt:
  lea     DosName(pc),a1         /* 'dos.library',0 */
  jsr     LVOFindResident(a6)    /* find the DOS resident tag */
  move.l  d0,a0                  /* in order to bootstrap */
  move.l  RT_INIT(A0),a0         /* set vector to DOS INIT */
  jsr     (a0)                   /* and initialize DOS */
  rts

DiagPt:

/*
This is the DiagPt / DiagEntry routine.

a0: board address (i.e. 0x40000000)
a2: load address
a3: configdev (i.e. 00004858)
*/

/*
  struct ConfigDev {
    struct Node   cd_Node; // 0  2ptr,2byte,1ptr = 14byte
    UBYTE   cd_Flags; // 14
    UBYTE   cd_Pad; // 15
    struct ExpansionRom cd_Rom; // 16   16bytes
    APTR    cd_BoardAddr; // 32
    ...
  */

  lea cardbase(pc), a1
  move.l a0, (a1)
  lea loadaddr(pc), a1
  move.l a2, (a1)
  lea configdev(pc), a3
  move.l a3, (a1)

  lea saddrs(pc), a6
  jsr puts

  move.l a0, d0
  jsr puthex

  move.l a3, d0
  jsr puthex

  move.l a2, d0
  jsr puthex

  move.l (a2), d0
  jsr puthex
  move.l 4(a2), d0
  jsr puthex
  move.l 8(a2), d0
  jsr puthex
  move.l 12(a2), d0
  jsr puthex

  // zzusb.device is at loadaddr + 0x400
  add.l #0x400, a2
  move.l a2, d0
  jsr puthex

  move.l a2, a0
  jsr relocatehunk

  jsr puthex // segment 0

  move.l d0, d7
  move.l (d7), d0 // 00000a10 (size)
  jsr puthex
  move.l 4(d7), d0 // 07800098 (next?)
  jsr puthex

  // romtag here
  move.l 8(d7), d0
  jsr puthex
  move.l 12(d7), d0
  jsr puthex
  move.l 16(d7), d0
  jsr puthex
  move.l 20(d7), d0
  jsr puthex
  move.l 24(d7), d0
  jsr puthex
  move.l 28(d7), d0
  jsr puthex
  move.l d7, d0

  // add offset to find romtag
  add.l #8, d0
  move.l d0, a1
  moveq.l #0, d1 // no seglist. should we pass it?

  move.l 4, a6 // execbase
  jsr -102(a6) // InitResident
  // return code in d0
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
saddrs:
  .string "brda,cfgd,ladr,[ladr]:\r\n"

.align 4
puts:
  // a6: string ptr
  // a5, d6: clobbered
  move.l cardbase(pc), a5
  moveq #0, d6
putsloop:
  move.b (a6)+, d6
  move.w d6, 0xf0(a5)
  bne putsloop
  rts

.align 4
puthex:
  // d0: long to print
  // a5: clobbered
  move.l cardbase(pc), a5
  swap d0
  move.w d0, 0xf2(a5)
  swap d0
  move.w d0, 0xf2(a5)
  move.w #0x0d, 0xf0(a5) // cr
  move.w #0x0a, 0xf0(a5) // lf
  rts

// https://github.com/cahirwpz/demoscene/blob/master/bootloader.asm
// Copyright 2020 Krystian Bacławski

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

/*
 Arguments:
   [a0] executable file image

 Result:
   [d0] linked list of relocated hunks
*/

relocatehunk:
	// executable amiga hunk file?
	cmp.l   #HUNK_HEADER,(a0)+
	bne     Panic

	movem.l d2-d4/a2-a3,-(sp)

	// assume there's no resident library name (skip long)
	lea			4(a0),a2
	// read number of hunks (n)
	move.l	(a2)+,d2
	// assume first hunk is 0 and last n-1 (skip two longs)
	addq.l	#8,a2

	// prepare for reading hunk specifiers (size, memory type)
	lsl.l		#2,d2						// [d2] hunk pointer array size
	sub.l		d2,sp						// [sp] hunk pointer array

  // allocate hunks in reverse order to make their addresses grow
  move.l  d2,d4 // [d2] number of hunks

.alloc:
  lea deballoc, a6
  jsr puts

  move.l  -4(a2,d4.l),d0  // d4 = index of current hunk, a2 = where we are in the file
  rol.l   #2,d0           // [d0] hunk size, bit(1) FAST, bit(0) CHIP
  moveq.l #1,d1
  and.l   d0,d1
  and.w   #-4,d0 // remove bits 1:0? isn't this too late?
  addq.l  #SEG_SIZE,d0 // plus header size
  jsr puthex // trace

  move.l  d0,d3
  add.l  #MEMF_CLEAR,d1
  move.l 4,a6
  jsr AllocMem(a6)

  // size is in d0
  // we get the pointer in d0
  jsr puthex // trace

  move.l  d0,-4(sp,d4.l)
  move.l  d0,a0
  move.l  d3,SEG_LEN(a0)
  cmp.l   d2,d4
  beq     .first
  move.l  (sp,d4.l),SEG_NEXT(a0)

.first:
  lea debfirst, a6
  jsr puts

  subq.l  #4,d4
  bgt     .alloc

  add.l   d2,a2           // [a2] first hunk header

  // parse hunks
  move.l  sp,a3
  move.l  d2,d4

.parse:
  lea debparse, a6
  jsr puts

  move.w  (a2)+,d0        // should always read zero
  add.w   (a2)+,d0        // hunk type

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
  moveq   #0,d0           // report error
  bra     .error

.hdata:
  lea debhdata, a6
  jsr puts

  move.l  (a3),a1
  add.w   #SEG_START,a1
  move.l  (a2)+,d0
  lsl.l   #2,d0           // [d0] hunk size
  move.l  a2,a0
  add.l   d0,a2           // move pointer to next hunk
  bsr     CopyMem
  // a0 = source, a1 = dest, d0 = number of longs
  bra     .parse

.hbss:
  lea debhbss, a6
  jsr puts

  addq.l  #4,a2           // skip bss length
  bra     .parse

.hsyms:
  lea debhsyms, a6
  jsr puts

  move.l  (a2)+,d0
  beq     .parse
  addq.l  #1,d0
  lsl.l   #2,d0
  add.l   d0,a2           // move pointer to symbol
  bra     .hsyms

.hreloc:
  lea debhreloc, a6
  jsr puts

  move.l  (a2)+,d3        // number of relocations
  beq     .parse          // done (zero)
  move.l  (a2)+,d0        // referenced hunk number
  lsl.l   #2,d0           // *4
  move.l  (sp,d0.l),d0    // get address from stack+hunk_number*4
  addq.l  #SEG_START,d0   // [d0] referenced hunk data address
  move.l  (a3),a1         // pointer to reloc table?!

  move.l d0,d7
  jsr puthex // print d0 = target segment
  move.l a1,d0
  jsr puthex // print a1 = relocation table in this segment
  move.l d7,d0

.reloc:
  move.l  (a2)+,d1
  add.l   d0,SEG_START(a1,d1.l)

  move.l d0,d7
  move.l d1,d0
  jsr puthex // print d1 = offset of relocation in patch table
  move.l a1, d0
  add.l d1, d0
  add.l SEG_START, d0
  jsr puthex // print modified address
  move.l SEG_START(a1,d1.l), d0
  jsr puthex // print modified data
  move.l d7,d0

  subq.l  #1,d3 // number of relocations--
  bgt     .reloc
  bra     .hreloc

.hend:
  lea debhend, a6
  jsr puts

  addq.l  #4,a3           // go to next hunk
  subq.l  #4,d4
  bne     .parse

.quit:
  move.l  (sp),d0

.error:
  add.l   d2,sp           // deallocate hunk array

  movem.l (sp)+,d2-d4/a2-a3
  rts

Panic:
  move.l d0, 0xdff180
  subq.l #1, d0
  bra Panic

/*
   [a0] pointer to source block
   [a1] pointer to destination block
   [d0] number of 4B words
*/

CopyMem:
  move.l  (a0)+,(a1)+
  subq.l  #4,d0
  bgt     CopyMem
  rts

  .align 4
hunkbase:
  .long 0

deballoc:
  .string "alloc\r\n"
debfirst:
  .string "first\r\n"
debparse:
  .string "parse\r\n"
debhdata:
  .string "hdata\r\n"
debhbss:
  .string "hbss\r\n"
debhsyms:
  .string "hsyms\r\n"
debhreloc:
  .string "hreloc\r\n"
debhend:
  .string "hend\r\n"
