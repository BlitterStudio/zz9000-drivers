;*---------------------------------------------------------------------------
;  :Module.	harness.s
;  :Contents.	Test harness for zz9000.s: the driver, assembled as a flat blob
;		without WHDLoad.Audio, and stubs for the operating system
;  :Copyright.	Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
;		SPDX-License-Identifier: GPL-3.0-or-later
;  :Language.	68000 Assembler
;  :Translator.	Vasm
;---------------------------------------------------------------------------*
; The blob is loaded by zz9000_audio_test.c, which runs it under qemu-m68k.
; Every library call of the driver goes through a stub that calls back into
; the C test (h_cfn). The test plays the part of exec, expansion.library,
; dos.library, zz9k.library and of the firmware behind the mailbox.
; The first long of the blob is the offset of h_ctl, a table of offsets
; and values for the test (see enum ctl in the C source).

	INCLUDE	exec/types.i
	INCLUDE	libraries/configvars.i
	INCLUDE	lvo/exec.i
	INCLUDE	whdload.i
	INCLUDE	whdaudio.i

VARS_SIZEOF	= 320
ZZ9000_TEST	= 1

	SECTION	harness,CODE

h_blob		dc.l	h_ctl-h_blob

;----------------------------------------
; call a routine with sentinels in all registers it must preserve
; C: ULONG h_callchk(fn, d0, d1, a0, ULONG *bad)
; bad gets a bit for each register which has been changed:
; bit 0 = d2 ... bit 5 = d7, bit 6 = a2 ... bit 10 = a6

h_callchk	movem.l	d2-d7/a2-a6,-(a7)
		move.l	(48,a7),a1
		move.l	(52,a7),d0
		move.l	(56,a7),d1
		move.l	(60,a7),a0
		move.l	#$d2d2d2d2,d2
		move.l	#$d3d3d3d3,d3
		move.l	#$d4d4d4d4,d4
		move.l	#$d5d5d5d5,d5
		move.l	#$d6d6d6d6,d6
		move.l	#$d7d7d7d7,d7
		move.l	#$a2a2a2a2,a2
		move.l	#$a3a3a3a3,a3
		move.l	#$a4a4a4a4,a4
		move.l	#$a5a5a5a5,a5
		move.l	#$a6a6a6a6,a6
		jsr	(a1)
		move.l	d0,-(a7)		;result
		moveq	#0,d0
		cmp.l	#$d2d2d2d2,d2
		beq	.a
		bset	#0,d0
.a		cmp.l	#$d3d3d3d3,d3
		beq	.b
		bset	#1,d0
.b		cmp.l	#$d4d4d4d4,d4
		beq	.c
		bset	#2,d0
.c		cmp.l	#$d5d5d5d5,d5
		beq	.d
		bset	#3,d0
.d		cmp.l	#$d6d6d6d6,d6
		beq	.e
		bset	#4,d0
.e		cmp.l	#$d7d7d7d7,d7
		beq	.f
		bset	#5,d0
.f		cmp.l	#$a2a2a2a2,a2
		beq	.g
		bset	#6,d0
.g		cmp.l	#$a3a3a3a3,a3
		beq	.h
		bset	#7,d0
.h		cmp.l	#$a4a4a4a4,a4
		beq	.i
		bset	#8,d0
.i		cmp.l	#$a5a5a5a5,a5
		beq	.j
		bset	#9,d0
.j		cmp.l	#$a6a6a6a6,a6
		beq	.k
		bset	#10,d0
.k		move.l	(68,a7),a1		;bad, the result is on the stack too
		move.l	d0,(a1)
		move.l	(a7)+,d0
		movem.l	(a7)+,d2-d7/a2-a6
		rts

;============================================================================
; calls back into C: void cfn(ULONG id, ULONG *regs), regs = d0,d1,a0,a1
; the C function sets the registers for the return

h_common	movem.l	d0-d1/a0-a1,-(a7)
		move.l	a7,-(a7)
		move.l	(20,a7),-(a7)
		move.l	(h_cfn,pc),a0
		jsr	(a0)
		addq.l	#8,a7
		movem.l	(a7)+,d0-d1/a0-a1
		addq.l	#4,a7
		rts

STUB		MACRO
h_stub\1	pea	\1.w
		bra.w	h_common
		ENDM

		STUB	0
		STUB	1
		STUB	2
		STUB	3
		STUB	4
		STUB	5
		STUB	6
		STUB	7
		STUB	8
		STUB	9
		STUB	10

; the driver calls _trace with a0 = format, a1 = arguments
_trace		bra.w	h_stub0

;============================================================================
; data

		CNOP	0,4
h_ctl
h_cfn		dc.l	0			;0 C function for the stubs
zz9000_test_exec dc.l	0			;1 exec base
		dc.l	zz9000_init-h_blob	;2
		dc.l	zz9000_exit-h_blob	;3
		dc.l	zz9000_play-h_blob	;4
		dc.l	zz9000_stop-h_blob	;5
		dc.l	zz9000_suspend-h_blob	;6
		dc.l	zz9000_resume-h_blob	;7
		dc.l	zz9000_volume-h_blob	;8
		dc.l	zz9000_irq-h_blob	;9
		dc.l	h_callchk-h_blob	;10
		dc.l	0			;11 (was the interrupt server)
		dc.l	_vars-h_blob		;12
		dc.l	zz9000_driver-h_blob	;13
		dc.l	h_stub0-h_blob		;14 stubs, trace first
		dc.l	h_stub1-h_blob
		dc.l	h_stub2-h_blob
		dc.l	h_stub3-h_blob
		dc.l	h_stub4-h_blob
		dc.l	h_stub5-h_blob
		dc.l	h_stub6-h_blob
		dc.l	h_stub7-h_blob
		dc.l	h_stub8-h_blob
		dc.l	h_stub9-h_blob
		dc.l	h_stub10-h_blob		;24
		dc.l	cd_BoardAddr		;25
		dc.l	cd_BoardSize		;26
		dc.l	0			;27 (unused)
		dc.l	0			;28
		dc.l	0			;29
		dc.l	0			;30
		dc.l	LIB_VERSION		;31
		dc.l	LIB_REVISION		;32
		dc.l	zv_zz9k			;33 offsets in the variables
		dc.l	zv_hw			;34
		dc.l	zv_ring			;35
		dc.l	zv_ctrl			;36
		dc.l	zv_cap			;37
		dc.l	zv_gen			;38
		dc.l	zv_slot			;39
		dc.l	zv_wrhi			;40
		dc.l	zv_wrlo			;41
		dc.l	zv_hb			;42
		dc.l	zv_pflags		;43
		dc.l	zv_vol			;44
		dc.l	zv_state		;45
		dc.l	zv_finpend		;46
		dc.l	zv_live			;47
		dc.l	zv_dead			;48
		dc.l	zv_armed		;49
		dc.l	0			;50 (unused)
		dc.l	zv_mem			;51
		dc.l	0			;52 (unused)
		dc.l	zv_st_tracks		;53
		dc.l	zv_st_refused		;54
		dc.l	zv_st_irqs		;55
		dc.l	zv_deadstat		;56
		dc.l	zv_left			;57
		dc.l	zv_total		;58
		dc.l	zv_fmt			;59
		dc.l	zv_SIZEOF		;60
		dc.l	VARS_SIZEOF		;61
		dc.l	zv_off			;62
		dc.l	zv_pad			;63
		dc.l	zv_st_susp		;64
		dc.l	zv_st_resume		;65

		CNOP	0,4
_vars		dcb.b	VARS_SIZEOF,0

;============================================================================

	INCLUDE	zz9000.s

	END
