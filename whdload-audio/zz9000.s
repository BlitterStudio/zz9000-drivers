;*---------------------------------------------------------------------------
;  :Module.	zz9000.s
;  :Contents.	ZZ9000AX driver for the WHDLoad audio plugin WHDLoad.Audio
;  :Copyright.	Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
;		SPDX-License-Identifier: GPL-3.0-or-later
;  :Language.	68000 Assembler
;  :Translator.	Vasm
;---------------------------------------------------------------------------*
; included by WHDLoad.Audio.asm (see build.sh), the variables are located at
; _vars which has a size of VARS_SIZEOF
;
; The driver takes one direct-ring lease while WHDLoad runs. Firmware with
; codec rings decodes MP3, FLAC and Ogg Vorbis; WAV is converted here to
; 44100 Hz stereo S16LE. Older firmware gets the same PCM over a source-rate
; lease. Firmware converts the source to its 48 kHz output. The card is told
; to raise its vblank interrupt, which
; reaches the level 2 interrupt (INT2 mode of the card, ENV:ZZ9K_INT2 or
; int2 = on in ZZ9000.CFG), and waud_irq then
;   - acknowledges the card interrupt,
;   - reads the credit line of the firmware (consumed cursor),
;   - copies the next part of the track into the ring and
;   - publishes the producer line with a new heartbeat.
; No custom or cia register is accessed. The ring and the control block are
; reported to WHDLoad as non cachable serialized memory, so the driver does
; not need any cache operation while the operating system is off.
;
; Whenever the driver stops feeding (state idle) and gives up the card
; interrupt, the producer line says PAUSED first: after waud_stop, a refused
; track (also one that replaces a playing track), waud_suspend, and at the
; end of a track once the card has played it. Only a paused lease has no
; heartbeat timeout.
;
; The card interrupt is enabled while a track is active, or for one interrupt
; to report a refusal. waud_stop, waud_suspend, IRQ completion and waud_exit
; disable it. WHDLoad gives the operating system back after waud_stop, and
; its PORTS chain does not know the vblank interrupt of the card.
;
; The driver leaves nothing in the operating system behind waud_init: no
; interrupt server, no task, no interrupt. While WHDLoad has the operating
; system active (loading files) waud_irq is not called, so nothing publishes
; a heartbeat. The lease is acquired with IDLE_PAUSED, which makes the
; firmware stop the 2000 ms heartbeat timeout while the producer line says
; PAUSED. The line is PAUSED from waud_init until the first track, between
; tracks, and from waud_suspend until waud_resume, so the timeout only runs
; while a track is playing and the level 2 interrupt is served.
;
; Firmware interface: docs/audio-fabric.md and sdk/include/zz9k/abi.h of
; zz9000-firmware (ZZ9KAudioRing*), zz9k.library revision 2.44 or later
;
; All routines called by WHDLoad modify d0/d1/a0/a1 only. Internal routines
; get the variables in a3 and keep all registers unless stated otherwise.

	IFND LIBRARIES_CONFIGVARS_I
	INCLUDE	libraries/configvars.i
	ENDC
	IFND LIBRARIES_EXPANSION_LVO_I
	INCLUDE	lvo/expansion.i
	ENDC
	IFND DOS_DOS_I
	INCLUDE	dos/dos.i
	ENDC
	IFND LVO_DOS_I
	INCLUDE	lvo/dos.i
	ENDC

;############################################################################

	;ZZ9000 hardware, include/zz9000_hw.h of zz9000-drivers
ZZ_MANUFACTURER		= $6d6e
ZZ_PRODUCT_Z2		= 3
ZZ_PRODUCT_Z3		= 4
ZZ_REG_CONFIG		= $04		;read: interrupt status, write: control
ZZ_REG_CFGKEY		= $e8		;ZZ9000.CFG query
ZZ_REG_CFGPRESENT	= $ea
ZZ_CFGKEY_INT2		= 5
ZZ_INTB_VBLANK		= 2		;bit in the interrupt status word (4)
ZZ_CFG_VBL_ON		= $82		;vblank-only mode, enable (bit 7, bit 1)
ZZ_CFG_VBL_OFF		= $80		;vblank-only mode, disable
ZZ_CFG_ACK_VBL		= $48		;acknowledge mode (8), vblank (64)
ZZ_REGS_SIZE		= $1000		;registers reported to WHDLoad

	;zz9k.library and mailbox, sdk/include/zz9k/abi.h
ZZ9K_LVO_QueryCaps	= -30
ZZ9K_LVO_Call		= -48
ZZ9K_LIB_VERSION	= 2
ZZ9K_LIB_REVISION	= 44
ZZ9K_TIMEOUT_TICKS	= 250		;ZZ9K_DEFAULT_TIMEOUT_TICKS
ZZ9K_ENTRY_INLINE	= 1
ZZ9K_OP_QUERY_SERVICE	= $0004
ZZ9K_OP_RING_ACQUIRE	= $0513
ZZ9K_OP_RING_RELEASE	= $0514
ZZ9K_SERVICE_AUDIO	= $0500
ZZ9K_CAPB_AUDIO_FABRIC	= 27
ZZ9K_SFB_FABRIC		= 22
ZZ9K_SFB_FABRIC_RATE	= 23
ZZ9K_IDENTITY_MEDIA	= 2
ZZ9K_ACQ_SOURCE_RATE	= 1
ZZ9K_ACQ_IDLE_PAUSED	= 4		;no heartbeat timeout while PAUSED
ZZ9K_CONTRACT_SOURCE_RATE = 2		;stereo S16LE at the source rate
ZZ9K_ACQ_CODEC		= 8
ZZ9K_CONTRACT_CODEC	= 8
ZZ9K_RING_PERIOD_BYTES	= 3840
ZZ9K_RING_PERIOD_US	= 20000
ZZ9K_RING_CONTROL_SIZE	= 128
ZZ9K_RING_PAUSED	= 1		;producer line flag

	;codec ring extension of the producer and firmware lines (BE32)
ZZ_P_STREAM		= 24		;marker serial, 0 until first marker
ZZ_P_CODEC		= 28
ZZ_P_STARTHI		= 32
ZZ_P_STARTLO		= 36
ZZ_P_STREAMFLAGS	= 40
ZZ_P_GAIN		= 48		;offset 44 (PCM rate) stays zero: 44100
ZZ_F_STREAM		= 20
ZZ_F_DECODE		= 24
ZZ_F_ERROR		= 28
ZZ_STREAM_APPEND	= 1
ZZ_STREAM_END		= 2
ZZ_DECODE_DRAINED	= 2
ZZ_DECODE_ERROR		= 3
ZCODEC_NONE		= -1
ZCODEC_WAV		= 0		;raw stereo S16LE in the ring
ZCODEC_MP3		= 1
ZCODEC_FLAC		= 2
ZCODEC_VORBIS		= 3

	;last refusal, kept until the OS can write a trace
ZRS_UNKNOWN		= 1
ZRS_OPUS		= 2
ZRS_OGG			= 3
ZRS_ASF			= 4
ZRS_MP4			= 5
ZRS_AAC			= 6
ZRS_WAV			= 7
ZRS_NO_CODEC		= 8
ZRS_LEASE		= 9
ZZ_MPEGSCAN		= 4096		;look past padding after an ID3v2 tag only

ZZ_SRCRATE		= 44100		;rate of the lease and of the tracks
ZZ_PERIOD		= ZZ_SRCRATE/50*4	;bytes of one 20 ms source period
ZZ_PREROLL		= 6*ZZ9K_RING_PERIOD_BYTES	;bytes before the firmware plays
ZZ_LEAD			= 12*ZZ_PERIOD	;bytes the producer keeps ahead
ZZ_CALLMAX		= 3*ZZ_PERIOD		;bytes written per call at most
ZZ_RINGMIN		= 8*ZZ9K_RING_PERIOD_BYTES

ZZ_CACRF_CLEARD		= $0800

	;states of the track
ZST_IDLE		= 0		;nothing to feed
ZST_PLAY		= 1		;audio data are fed
ZST_PAD			= 2		;silence is fed to complete the last period
ZST_FIN			= 3		;everything fed, wait until the card played it

	;frame of init/exit
	STRUCTURE	ZZ9000Frame,0
		STRUCT	zf_req,64		;mailbox request
		STRUCT	zf_rep,64		;mailbox reply
		STRUCT	zf_caps,64		;ZZ9KCaps
		STRUCT	zf_args,32		;trace arguments
		LABEL	zf_SIZEOF

	STRUCTURE	ZZ9000Vars,0
		APTR	zv_zz9k			;zz9k.library, <>0 = open
		APTR	zv_hw			;board address
		ULONG	zv_hwsize		;board size
		APTR	zv_ring			;PCM ring
		APTR	zv_ctrl			;control block, producer line at +0,
						;firmware line at +64
		ULONG	zv_cap			;capacity of the ring in bytes
		ULONG	zv_gen			;generation of the lease
		ULONG	zv_slot			;slot of the lease
		ULONG	zv_wrhi			;write cursor (bytes since acquire)
		ULONG	zv_wrlo
		ULONG	zv_seq			;sequence of the producer line (even)
		ULONG	zv_hb			;heartbeat token
		ULONG	zv_pflags		;producer line flags
		ULONG	zv_off			;write position in the ring
		ULONG	zv_phase		;write cursor mod ZZ_PERIOD
		ULONG	zv_vol			;volume 0..$7fff
		ULONG	zv_pflg			;WHDLAUDF_* of the track
		APTR	zv_start		;first sample (WAV) or byte (compressed)
		APTR	zv_ptr			;next sample or byte to feed
		ULONG	zv_left			;frames (WAV) or bytes (compressed) left
		ULONG	zv_total		;frames or bytes in the track
		ULONG	zv_pad			;bytes of silence left
		ULONG	zv_deadstat		;firmware status of the lost lease
		ULONG	zv_st_tracks		;statistics, traced on exit
		ULONG	zv_st_refused
		ULONG	zv_st_irqs
		ULONG	zv_st_susp
		ULONG	zv_st_resume
		ULONG	zv_dbgcnt		;ZZ9000_DIAG: progress codes left
		ULONG	zv_stream		;current marker serial
		ULONG	zv_codec		;current marker codec
		ULONG	zv_starthi		;cursor at the marker boundary
		ULONG	zv_startlo
		ULONG	zv_streamflags		;APPEND, END
		ULONG	zv_seen			;last firmware stream serial read
		ULONG	zv_decode		;last decode state read
		ULONG	zv_error		;decode error read with the state
		ULONG	zv_last_error		;last track decoder error, for exit trace
		ULONG	zv_refusal		;ZRS_*, for exit trace
		UBYTE	zv_fmt			;0 s16 stereo, 1 s16 mono, 2 u8 stereo, 3 u8 mono
		UBYTE	zv_shift		;log2 of the bytes of a frame
		UBYTE	zv_state		;ZST_*
		UBYTE	zv_finpend		;WAUDIRQF_FINISHED to report
		UBYTE	zv_live			;lease held
		UBYTE	zv_dead			;lease lost
		UBYTE	zv_armed		;card interrupt enabled
		UBYTE	zv_diag			;ZZ9000_DIAG: ZZ_DIAG_* switches
		UBYTE	zv_codecmode		;codec lease rather than legacy PCM lease
		STRUCT	zv_pad1,3
		STRUCT	zv_mem,(3*3+1)*4	;memory list for WHDLoad
		LABEL	zv_SIZEOF

	IFLT VARS_SIZEOF-zv_SIZEOF
	FAIL	VARS_SIZEOF is too small for ZZ9000Vars
	ENDC

ZZ_GETEXEC	MACRO
	IFD ZZ9000_TEST
		move.l	(zz9000_test_exec,pc),\1
	ELSE
		move.l	(4).w,\1
	ENDC
	ENDM

	;Diagnostic build (ZZ9000_DIAG, build.sh diag): progress codes are written
	;as characters to REG_ZZ_PRINT_CHR, which the firmware prints on its UART,
	;and ENV:ZZ9K_WA_* files switch parts of the driver off, see README.md
ZZ_REG_PRINT		= $f0
ZZ_DIAG_NORESUME	= 0		;waud_resume does not enable the card interrupt
ZZ_DIAG_NOIRQ		= 2		;waud_irq returns at once
ZZ_DIAG_NOARM		= 3		;the card interrupt is never enabled
ZZ_DBG		MACRO
	IFD ZZ9000_DIAG
		move.l	d0,-(a7)
		moveq	#\1,d0
		bsr	zzi_dbg
		move.l	(a7)+,d0
	ENDC
		ENDM
ZZ_DBGL		MACRO
	IFD ZZ9000_DIAG
		move.l	d0,-(a7)
		moveq	#\1,d0
		bsr	zzi_dbgl
		move.l	(a7)+,d0
	ENDC
		ENDM

	CNOP 0,4
zz9000_driver	dc.l	zz9000_name
		dc.l	zz9000_init
zz9000_driver_exts dc.l	zz9000_exts
		dc.l	_vars+zv_mem
		dc.l	zz9000_exit
		dc.l	zz9000_play
		dc.l	zz9000_stop
		dc.l	zz9000_suspend
		dc.l	zz9000_resume
		dc.l	zz9000_volume
		dc.l	zz9000_irq

;----------------------------------------
; detect the ZZ9000, check the firmware and take the lease
; IN:	-
; OUT:	d0 = LONG WAUDINIT_*
;	a0 = CPTR error message

zz9000_init	movem.l	d2-d7/a2-a6,-(a7)
		lea	(-zf_SIZEOF,a7),a7
		move.l	a7,a4			;A4 = frame
		lea	(_vars,pc),a3
		moveq	#WAUDINIT_NOHW,d7	;D7 = result
		sub.l	a5,a5			;A5 = error message
		ZZ_GETEXEC a2			;A2 = exec
		move.l	#$7fff,(zv_vol,a3)	;vars are cleared

	;find the board, Zorro III first
		move.l	a2,a6
		lea	(zz9000_n_expansion,pc),a1
		moveq	#0,d0
		jsr	(_LVOOpenLibrary,a6)
		lea	(zz9000_t_noexp,pc),a0
		move.l	d0,d6			;D6 = expansion.library
		beq	.trace
		move.l	d0,a6
		sub.l	a0,a0
		move.l	#ZZ_MANUFACTURER,d0
		moveq	#ZZ_PRODUCT_Z3,d1
		jsr	(_LVOFindConfigDev,a6)
		moveq	#1,d5			;D5 = <>0 Zorro III
		tst.l	d0
		bne	.cd
		sub.l	a0,a0
		move.l	#ZZ_MANUFACTURER,d0
		moveq	#ZZ_PRODUCT_Z2,d1
		jsr	(_LVOFindConfigDev,a6)
		moveq	#0,d5
.cd		move.l	d0,d2			;D2 = ConfigDev
		beq	.cdclose
		move.l	d0,a0
		move.l	(cd_BoardAddr,a0),(zv_hw,a3)
		move.l	(cd_BoardSize,a0),(zv_hwsize,a3)
.cdclose	move.l	d6,a1
		move.l	a2,a6
		jsr	(_LVOCloseLibrary,a6)
		tst.l	d2
		lea	(zz9000_t_nocard,pc),a0
		beq	.trace
		tst.l	d5
		lea	(zz9000_t_z2,pc),a0
		beq	.trace

	;the card interrupt must reach the level 2 interrupt, ENV:ZZ9K_INT2 or
	;int2 = on in ZZ9000.CFG (include/zzcfg_query.h)
		moveq	#0,d6			;D6 = <>0 INT2 mode
		move.l	a2,a6
		lea	(zz9000_n_dos,pc),a1
		moveq	#0,d0
		jsr	(_LVOOpenLibrary,a6)
		move.l	d0,d5			;D5 = dos.library
		beq	.cfg
		move.l	d0,a6
		lea	(zz9000_n_int2env,pc),a0
		move.l	a0,d1
		move.l	#MODE_OLDFILE,d2
		jsr	(_LVOOpen,a6)
		move.l	d0,d1
		beq	.envdone
		jsr	(_LVOClose,a6)
		moveq	#1,d6
.envdone	move.l	d5,a1
		move.l	a2,a6
		jsr	(_LVOCloseLibrary,a6)
		tst.l	d6
		bne	.int2ok
.cfg		move.l	(zv_hw,a3),a0
		move	#ZZ_CFGKEY_INT2,(ZZ_REG_CFGKEY,a0)
		move	(ZZ_REG_CFGKEY,a0),d0
		move	(ZZ_REG_CFGPRESENT,a0),d1
		tst	d1
		lea	(zz9000_t_noint2,pc),a0
		beq	.trace
		tst	d0
		beq	.trace
.int2ok		ZZ_DBG	'C'
	IFD ZZ9000_DIAG
		bsr	zzi_envflags
	ENDC
	;zz9k.library 2.44
		move.l	a2,a6
		lea	(zz9000_n_zz9k,pc),a1
		moveq	#ZZ9K_LIB_VERSION,d0
		jsr	(_LVOOpenLibrary,a6)
		move.l	d0,(zv_zz9k,a3)
		lea	(zz9000_t_nolib,pc),a0
		beq	.trace
		move.l	d0,a6			;A6 = zz9k.library
		move.l	d0,a0
		cmp	#ZZ9K_LIB_VERSION,(LIB_VERSION,a0)
		bhi	.libok
		moveq	#0,d0
		move	(LIB_REVISION,a0),d0
		cmp	#ZZ9K_LIB_REVISION,d0
		bhs	.libok
		move.l	d0,(zf_args,a4)
		lea	(zz9000_t_oldlib,pc),a0
		lea	(zf_args,a4),a1
		bra	.fail_lib
.libok		ZZ_DBG	'Z'
	;the audio fabric
		lea	(zf_caps,a4),a0
		jsr	(ZZ9K_LVO_QueryCaps,a6)
		lea	(zz9000_t_nofabric,pc),a0
		sub.l	a1,a1
		tst.l	d0
		bne	.fail_lib
		move.l	(zf_caps+8,a4),d0	;capability_bits
		btst	#ZZ9K_CAPB_AUDIO_FABRIC,d0
		beq	.fail_lib
		move	#ZZ9K_OP_QUERY_SERVICE,d0
		bsr	zzi_req
		move.l	#ZZ9K_SERVICE_AUDIO,(zf_req+16,a4)
		bsr	zzi_call
		lea	(zz9000_t_nofabric,pc),a0
		sub.l	a1,a1
		tst.l	d0
		bne	.fail_lib
		move.l	(zf_rep+16+12,a4),d0	;flags
		btst	#ZZ9K_SFB_FABRIC,d0
		beq	.fail_lib
		lea	(zz9000_t_norate,pc),a0
		btst	#ZZ9K_SFB_FABRIC_RATE,d0
		beq	.fail_lib

	;try a codec lease first; BAD_REQUEST means try a PCM lease in that slot
		moveq	#ZZ9K_ACQ_CODEC|ZZ9K_ACQ_IDLE_PAUSED,d5
		moveq	#1,d4			;D4 = slot
.slot		move	#ZZ9K_OP_RING_ACQUIRE,d0
		bsr	zzi_req
		move.l	d4,(zf_req+16,a4)	;slot
		move.l	#ZZ9K_IDENTITY_MEDIA,(zf_req+20,a4)
		move.l	#128,(zf_req+24,a4)	;gain, 128 = unity
		move.l	d5,(zf_req+28,a4)
		btst	#0,d5			;SOURCE_RATE
		beq	.call
		move.l	#ZZ_SRCRATE,(zf_req+32,a4)
.call
		bsr	zzi_call
		tst.l	d0
		beq	.acquired
		cmp.l	#4,d0			;ZZ9K_STATUS_BAD_REQUEST
		bne	.nextslot
		btst	#3,d5			;CODEC
		beq	.nextslot
		moveq	#ZZ9K_ACQ_SOURCE_RATE|ZZ9K_ACQ_IDLE_PAUSED,d5
		bra	.slot
.nextslot
		move.l	d0,d3			;D3 = last status
		addq	#1,d4
		cmp	#2,d4
		bls	.slot
		move.l	d3,(zf_args,a4)
		lea	(zz9000_t_busy,pc),a0
		cmp.l	#4,d3			;ZZ9K_STATUS_BAD_REQUEST
		bne	.busy
		lea	(zz9000_t_noidle,pc),a0
.busy		lea	(zf_args,a4),a1
		bra	.fail_lib

	;check the grant, reply payload at +16:
	;slot 0, generation 4, ring_offset 8, ring_capacity 12, control_offset 16,
	;period_bytes 20, period_us 24, sample_contract 28, gain 32, slot_count 36,
	;flags 40, source_rate 44
.acquired	btst	#3,d5			;CODEC
		sne	(zv_codecmode,a3)
		move.l	(zf_rep+16+4,a4),d3	;D3 = generation
		move.l	(zf_rep+16,a4),d4	;D4 = slot
		move.l	(zf_rep+16+8,a4),d5	;D5 = ring offset
		move.l	(zf_rep+16+12,a4),d6	;D6 = capacity
		move.l	(zf_rep+16+16,a4),d2	;D2 = control offset
		move.l	d4,(zf_args,a4)
		move.l	d3,(zf_args+4,a4)
		move.l	d5,(zf_args+8,a4)
		move.l	d6,(zf_args+12,a4)
		move.l	d2,(zf_args+16,a4)
		tst.l	d3
		beq	.badgrant
		cmp.l	#ZZ9K_RING_PERIOD_BYTES,(zf_rep+16+20,a4)
		bne	.badgrant
		cmp.l	#ZZ9K_RING_PERIOD_US,(zf_rep+16+24,a4)
		bne	.badgrant
		tst.b	(zv_codecmode,a3)
		beq	.pcmgrant
		cmp.l	#ZZ9K_CONTRACT_CODEC,(zf_rep+16+28,a4)
		bne	.badgrant
		tst.l	(zf_rep+16+44,a4)
		bne	.badgrant
		bra	.capacity
.pcmgrant
		cmp.l	#ZZ9K_CONTRACT_SOURCE_RATE,(zf_rep+16+28,a4)
		bne	.badgrant
		cmp.l	#ZZ_SRCRATE,(zf_rep+16+44,a4)
		bne	.badgrant
.capacity
		cmp.l	#ZZ_RINGMIN,d6
		blo	.badgrant
		cmp.l	#$100000,d6
		bhi	.badgrant
		move.l	d6,d0
		divu	#ZZ9K_RING_PERIOD_BYTES,d0	;quotient is small
		swap	d0
		tst	d0			;remainder
		bne	.badgrant
		move.l	d5,d0
		or.l	d2,d0
		and.l	#3,d0
		bne	.badgrant
		move.l	d2,d0
		and.l	#ZZ9K_RING_CONTROL_SIZE/2-1,d0	;control block 64 byte aligned
		bne	.badgrant
		move.l	(zv_hwsize,a3),d0
		move.l	d5,d1
		add.l	d6,d1
		bcs	.badgrant
		cmp.l	d0,d1			;ring end <= board size
		bhi	.badgrant
		move.l	d2,d1
		add.l	#ZZ9K_RING_CONTROL_SIZE,d1
		bcs	.badgrant
		cmp.l	d0,d1			;control end <= board size
		bhi	.badgrant
		move.l	d5,d0
		add.l	d6,d0			;ring end
		cmp.l	d2,d0
		bls	.nooverlap		;ring below control
		move.l	d2,d0
		add.l	#ZZ9K_RING_CONTROL_SIZE,d0	;control end
		cmp.l	d5,d0
		bhi	.badgrant		;control overlaps ring
.nooverlap	bra	.granted

.badgrant	move.l	d4,d0			;release what the firmware gave
		move.l	d3,d1
		bsr	zzi_release
		lea	(zz9000_t_badgrant,pc),a0
		lea	(zf_args,a4),a1
		bra	.fail_lib

	;the lease is ours
.granted	move.l	d4,(zv_slot,a3)
		move.l	d3,(zv_gen,a3)
		move.l	d6,(zv_cap,a3)
		move.l	(zv_hw,a3),d0
		add.l	d0,d5
		add.l	d0,d2
		move.l	d5,(zv_ring,a3)
		move.l	d2,(zv_ctrl,a3)
		move.l	#ZZ9K_RING_PAUSED,(zv_pflags,a3)
		moveq	#1,d0
		move.l	d0,(zv_hb,a3)
		st	(zv_live,a3)
		lea	(zz9000_exts,pc),a0
		tst.b	(zv_codecmode,a3)
		beq	.setexts
		lea	(zz9000_codecexts,pc),a0
.setexts	lea	(zz9000_driver_exts,pc),a1
		move.l	a0,(a1)			;plugin copies this pointer after init

	;memory for WHDLoad, uncached and serialized: registers, ring, control
		lea	(zv_mem,a3),a0
		moveq	#WAUDMEM_NCS,d1
		move.l	(zv_hw,a3),d0
		move.l	d0,(a0)+
		add.l	#ZZ_REGS_SIZE-1,d0
		move.l	d0,(a0)+
		move.l	d1,(a0)+
		move.l	d5,(a0)+
		move.l	d5,d0
		add.l	d6,d0
		subq.l	#1,d0
		move.l	d0,(a0)+
		move.l	d1,(a0)+
		move.l	d2,(a0)+
		move.l	d2,d0
		add.l	#ZZ9K_RING_CONTROL_SIZE-1,d0
		move.l	d0,(a0)+
		move.l	d1,(a0)+
		clr.l	(a0)

	;first publication, the lease is paused until a track is played
		ZZ_DBG	'L'
		bsr	zzi_publish
		move.l	a2,a6
		bsr	zzi_flush

		move.l	(zv_slot,a3),(zf_args,a4)
		move.l	(zv_gen,a3),(zf_args+4,a4)
		move.l	(zv_cap,a3),(zf_args+8,a4)
		move.l	(zv_ring,a3),(zf_args+12,a4)
		move.l	(zv_ctrl,a3),(zf_args+16,a4)
		lea	(zz9000_t_ok,pc),a0
		lea	(zf_args,a4),a1
		bsr	_trace
		moveq	#WAUDINIT_OK,d7
		ZZ_DBG	'O'
		bra	.end

.fail_lib	bsr	_trace
		move.l	(zv_zz9k,a3),a1
		move.l	a2,a6
		jsr	(_LVOCloseLibrary,a6)
		clr.l	(zv_zz9k,a3)
		bra	.end
.trace		sub.l	a1,a1
		bsr	_trace
.end		lea	(zf_SIZEOF,a7),a7
		move.l	d7,d0
		move.l	a5,a0
		movem.l	(a7)+,d2-d7/a2-a6
		rts

;----------------------------------------
; clear the request and reply of the frame and set the opcode
; IN:	a4 = APTR frame
;	d0 = UWORD opcode
; OUT:	d1/a0 destroyed

zzi_req		lea	(zf_req,a4),a0
		moveq	#(zf_caps-zf_req)/4-1,d1
.clr		clr.l	(a0)+
		dbf	d1,.clr
		move	d0,(zf_req+4,a4)
		move	#ZZ9K_ENTRY_INLINE,(zf_req+8,a4)
		move	#48,(zf_req+10,a4)	;inline payload
		rts

;----------------------------------------
; call the mailbox
; IN:	a4 = APTR frame
;	a6 = APTR zz9k.library
; OUT:	d0 = LONG status, ZZ9K_STATUS_OK = 0
;	d1/a0/a1 destroyed

zzi_call	lea	(zf_req,a4),a0
		lea	(zf_rep,a4),a1
		move.l	#ZZ9K_TIMEOUT_TICKS,d0
		jsr	(ZZ9K_LVO_Call,a6)
		rts

;----------------------------------------
; release a lease, the result is ignored, a stale generation is accepted
; IN:	a4 = APTR frame
;	a6 = APTR zz9k.library
;	d0 = ULONG slot
;	d1 = ULONG generation
; OUT:	d0-d1/a0-a1 destroyed

zzi_release	movem.l	d2-d3,-(a7)
		move.l	d0,d2
		move.l	d1,d3
		move	#ZZ9K_OP_RING_RELEASE,d0
		bsr	zzi_req
		move.l	d2,(zf_req+16,a4)
		move.l	d3,(zf_req+20,a4)
		bsr	zzi_call
		movem.l	(a7)+,d2-d3
		rts

;----------------------------------------
; flush the data cache over the control line, for the operating system
; mapping which may be cachable
; IN:	a3 = APTR vars
;	a6 = APTR exec
; OUT:	d0-d1/a0-a1 destroyed

zzi_flush	move.l	(zv_ctrl,a3),a0
		moveq	#64,d0
		move.l	#ZZ_CACRF_CLEARD,d1
		jsr	(_LVOCacheClearE,a6)
		rts

;----------------------------------------
; release the lease and close the library
; IN:	-
; OUT:	-

zz9000_exit	movem.l	d2-d7/a2-a6,-(a7)
		lea	(-zf_SIZEOF,a7),a7
		move.l	a7,a4			;A4 = frame
		lea	(_vars,pc),a3
		ZZ_GETEXEC a2
		ZZ_DBG	'X'
		bsr	zzi_disarm
		ZZ_DBG	'D'
		sf	(zv_live,a3)
		move.l	(zv_zz9k,a3),d0
		beq	.nolib
		move.l	d0,a6
		move.l	(zv_slot,a3),d0
		move.l	(zv_gen,a3),d1
		bsr	zzi_release
		ZZ_DBG	'E'
		moveq	#0,d0
		move.b	(zv_dead,a3),d0
		move.l	(zv_st_tracks,a3),(zf_args,a4)
		move.l	(zv_st_refused,a3),(zf_args+4,a4)
		move.l	(zv_st_irqs,a3),(zf_args+8,a4)
		move.l	(zv_st_susp,a3),(zf_args+12,a4)
		move.l	(zv_st_resume,a3),(zf_args+16,a4)
		lea	(zz9000_s_intact,pc),a1
		tst.l	d0
		beq	.stat
		lea	(zz9000_s_lost,pc),a1
.stat		move.l	a1,(zf_args+20,a4)
		move.l	(zv_deadstat,a3),(zf_args+24,a4)
		lea	(zz9000_t_stat,pc),a0
		lea	(zf_args,a4),a1
		bsr	_trace
		move.l	(zv_refusal,a3),d0
		beq	.decode
		add.w	d0,d0
		lea	(zz9000_refusals,pc),a0
		move.w	(-2,a0,d0.w),d0
		lea	(a0,d0.w),a0
		move.l	a0,(zf_args,a4)
		lea	(zz9000_t_refused,pc),a0
		lea	(zf_args,a4),a1
		bsr	_trace
.decode		move.l	(zv_last_error,a3),d0
		beq	.close
		move.l	d0,(zf_args,a4)
		lea	(zz9000_t_decode,pc),a0
		lea	(zf_args,a4),a1
		bsr	_trace
.close
		move.l	(zv_zz9k,a3),a1
		move.l	a2,a6
		jsr	(_LVOCloseLibrary,a6)
		clr.l	(zv_zz9k,a3)
		ZZ_DBG	'F'
.nolib		lea	(zf_SIZEOF,a7),a7
		movem.l	(a7)+,d2-d7/a2-a6
		rts

;----------------------------------------
; publish the producer line: sequence odd, generation, write cursor,
; heartbeat, flags, stream marker, sequence even
; IN:	a3 = APTR vars
; OUT:	d0/a0 destroyed

zzi_publish	move.l	(zv_ctrl,a3),a0
		move.l	(zv_seq,a3),d0
		addq.l	#2,d0
		move.l	d0,(zv_seq,a3)
		addq.l	#1,d0
		move.l	d0,(a0)			;sequence odd, update in flight
		move.l	(zv_gen,a3),(4,a0)
		move.l	(zv_wrhi,a3),(8,a0)
		move.l	(zv_wrlo,a3),(12,a0)
		addq.l	#1,(zv_hb,a3)
		move.l	(zv_hb,a3),(16,a0)
		move.l	(zv_pflags,a3),(20,a0)
		move.l	(zv_stream,a3),(ZZ_P_STREAM,a0)
		move.l	(zv_codec,a3),(ZZ_P_CODEC,a0)
		move.l	(zv_starthi,a3),(ZZ_P_STARTHI,a0)
		move.l	(zv_startlo,a3),(ZZ_P_STARTLO,a0)
		move.l	(zv_streamflags,a3),(ZZ_P_STREAMFLAGS,a0)
		move.l	(zv_vol,a3),(ZZ_P_GAIN,a0)
		move.l	(zv_seq,a3),(a0)	;sequence even, stable
		rts

;----------------------------------------
; read the firmware line: sequence, generation, consumed cursor, status
; a lease with another generation or a status other than OK is lost
; IN:	a3 = APTR vars
; OUT:	d0 = ULONG consumed cursor, low 32 bit
;	d1 = LONG 0 = valid, -1 = line was updating, no data
;	zv_dead is set if the lease is lost
;	a0 destroyed

zzi_credit	movem.l	d2-d7,-(a7)
		move.l	(zv_ctrl,a3),a0
		lea	(64,a0),a0
		moveq	#3,d4			;attempts
.try		move.l	(a0),d2			;sequence
		btst	#0,d2
		bne	.again			;update in flight
		move.l	(4,a0),d3		;generation
		move.l	(12,a0),d0		;consumed cursor, low word
		move.l	(16,a0),d1		;status
		move.l	(ZZ_F_STREAM,a0),d5
		move.l	(ZZ_F_DECODE,a0),d6
		move.l	(ZZ_F_ERROR,a0),d7
		cmp.l	(a0),d2
		beq	.stable
.again		dbf	d4,.try
		moveq	#-1,d1
		bra	.end
.stable		cmp.l	(zv_gen,a3),d3
		bne	.lost
		tst.l	d1
		bne	.lost
		move.l	d5,(zv_seen,a3)
		move.l	d6,(zv_decode,a3)
		move.l	d7,(zv_error,a3)
		bra	.end			;d1 = 0
.lost		st	(zv_dead,a3)
		move.l	d1,(zv_deadstat,a3)
		tst.b	(zv_state,a3)
		beq	.lost2
		st	(zv_finpend,a3)		;tell WHDLoad that the track ended
.lost2		clr.b	(zv_state,a3)
		moveq	#-1,d1
.end		movem.l	(a7)+,d2-d7
		rts

;----------------------------------------
; enable and disable the vblank interrupt of the card
; IN:	a3 = APTR vars
; OUT:	a0 destroyed

zzi_arm
	IFD ZZ9000_DIAG
		btst	#ZZ_DIAG_NOARM,(zv_diag,a3)
		bne	.rts
		ZZ_DBG	'+'
	ENDC
		move.l	(zv_hw,a3),a0
		move	#ZZ_CFG_VBL_ON,(ZZ_REG_CONFIG,a0)
		st	(zv_armed,a3)
.rts		rts

zzi_disarm	move.l	(zv_hw,a3),d0
		beq	.rts			;init failed early
		move.l	d0,a0
		move	#ZZ_CFG_VBL_OFF,(ZZ_REG_CONFIG,a0)
		sf	(zv_armed,a3)
.rts		rts

;----------------------------------------
; suspend/resume interrupts, the operating system becomes active or inactive
; IN:	-
; OUT:	-

zz9000_suspend	move.l	a3,-(a7)
		lea	(_vars,pc),a3
		addq.l	#1,(zv_st_susp,a3)
		ZZ_DBG	'S'
		bsr	zzi_disarm
		bsr	zzi_pause
		move.l	(a7)+,a3
		rts

zz9000_resume	move.l	a3,-(a7)
		lea	(_vars,pc),a3
		addq.l	#1,(zv_st_resume,a3)
		ZZ_DBG	'R'
	IFD ZZ9000_DIAG
		btst	#ZZ_DIAG_NORESUME,(zv_diag,a3)
		bne	.skip
	ENDC
		tst.b	(zv_state,a3)
		bne	.arm
		tst.b	(zv_finpend,a3)
		beq	.skip			;idle: no card interrupt
.arm		bsr	zzi_arm
.skip		tst.b	(zv_state,a3)
		beq	.end
		tst.b	(zv_dead,a3)
		bne	.end
		clr.l	(zv_pflags,a3)		;playing: unpaused again
		bsr	zzi_publish
.end		move.l	(a7)+,a3
		rts

;----------------------------------------
; pause the lease for the time the operating system is active, so that the
; firmware does not wait for a heartbeat
; IN:	a3 = APTR vars
; OUT:	d0/a0 destroyed

zzi_pause	tst.b	(zv_dead,a3)
		bne	.rts
		move.l	#ZZ9K_RING_PAUSED,(zv_pflags,a3)
		bsr	zzi_publish
.rts		rts

;----------------------------------------
; set volume
; IN:	d0 = ULONG volume 0..$7fff
; OUT:	-

zz9000_volume
	IFD ZZ9000_DIAG
		move.l	a3,-(a7)
		lea	(_vars,pc),a3
		ZZ_DBG	'v'
		move.l	(a7)+,a3
	ENDC
		cmp.l	#$7fff,d0
		bls	.set
		move.l	#$7fff,d0
.set		lea	(_vars+zv_vol,pc),a0
		move.l	d0,(a0)
		rts

;----------------------------------------
; stop playing, the lease stays paused and keeps its heartbeat
; IN:	-
; OUT:	-

zz9000_stop	movem.l	d0/a0/a3,-(a7)
		lea	(_vars,pc),a3
		ZZ_DBG	'T'
		clr.b	(zv_state,a3)
		clr.b	(zv_finpend,a3)
		bsr	zzi_disarm		;nothing to feed: no card interrupt
		bsr	zzi_flushtrack
.end		movem.l	(a7)+,d0/a0/a3
		rts

; Set a marker at the current write cursor. The caller publishes it along
; with the cursor on the producer line. Skip serial zero on wrap.
; IN: d0 = codec, d1 = stream flags, a3 = vars

zzi_marker	addq.l	#1,(zv_stream,a3)
		bne	.serial
		addq.l	#1,(zv_stream,a3)
.serial		move.l	d0,(zv_codec,a3)
		move.l	d1,(zv_streamflags,a3)
		move.l	(zv_wrhi,a3),(zv_starthi,a3)
		move.l	(zv_wrlo,a3),(zv_startlo,a3)
		rts

zzi_flushtrack	tst.b	(zv_dead,a3)
		bne	.rts
		moveq	#ZCODEC_NONE,d0
		moveq	#0,d1
		bsr	zzi_marker
		bra	zzi_pause
.rts		rts

;----------------------------------------
; start playing a track
; IN:	d0 = ULONG length of audio data
;	d1 = ULONG flags, WHDLAUDF_*
;	a0 = APTR audio data
; OUT:	-

zz9000_play	movem.l	d2-d7/a2-a3,-(a7)
		lea	(_vars,pc),a3
		ZZ_DBG	'P'
		clr.b	(zv_state,a3)
		clr.b	(zv_finpend,a3)
		clr.l	(zv_pad,a3)
		move.l	d1,(zv_pflg,a3)
		tst.b	(zv_dead,a3)
		bne	.dead
		move.l	a0,a2
		move.l	d0,d2			;file length
		bsr	zzi_detect
		tst.l	d0
		bmi	.refuse
		beq	.wav
		tst.b	(zv_codecmode,a3)
		beq	.nocodec
		move.l	a1,d1
		sub.l	a2,d1			;skip a tag or padding
		sub.l	d1,d2
		move.l	a1,(zv_start,a3)
		move.l	a1,(zv_ptr,a3)
		move.l	d2,(zv_total,a3)
		move.l	d2,(zv_left,a3)
		bra	.start
.wav		move.l	a2,a0
		move.l	d2,d0
		bsr	zzi_parse
		tst.l	d0
		bne	.badwav

	;The PCM kernels require an aligned destination. The compressed feed
	;leaves room for these discarded bytes even with the ring nearly full.
		move.l	(zv_off,a3),d1
		neg.l	d1
		and.l	#3,d1
		beq	.start
		move.l	d1,d0
		bsr	zzi_advance		;gap is below the new start, never read
		moveq	#ZCODEC_WAV,d0
.start		moveq	#0,d1
		bsr	zzi_marker		;flush previous bytes and start this track
		addq.l	#1,(zv_st_tracks,a3)
		clr.l	(zv_pflags,a3)		;unpaused
		move.b	#ZST_PLAY,(zv_state,a3)
		bsr	zzi_service		;prime the ring before enabling the IRQ
		bsr	zzi_arm
		bra	.end

	;not playable, the next interrupt reports the end
.dead		moveq	#-ZRS_LEASE,d0
		bra	.refuse
.nocodec	moveq	#-ZRS_NO_CODEC,d0
		bra	.refuse
.badwav		moveq	#-ZRS_WAV,d0
.refuse		neg.l	d0
		move.l	d0,(zv_refusal,a3)
		addq.l	#1,(zv_st_refused,a3)
		st	(zv_finpend,a3)
		bsr	zzi_flushtrack		;a playing track is replaced: discard it
		bsr	zzi_arm
.end		movem.l	(a7)+,d2-d7/a2-a3
		rts

;----------------------------------------
; interrupt routine
; IN:	-
; OUT:	d0 = ULONG WAUDIRQF_*

zz9000_irq	movem.l	d2-d7/a2-a3,-(a7)
		lea	(_vars,pc),a3
		moveq	#0,d7			;D7 = result
	IFD ZZ9000_DIAG
		btst	#ZZ_DIAG_NOIRQ,(zv_diag,a3)
		bne	.end
		ZZ_DBGL	'i'
	ENDC
		tst.b	(zv_armed,a3)
		beq	.fin
		move.l	(zv_hw,a3),a2
		move	(ZZ_REG_CONFIG,a2),d0
	IFD ZZ9000_DIAG
		bsr	zzi_dbghexl
	ENDC
		btst	#ZZ_INTB_VBLANK,d0	;our interrupt?
		beq	.fin
		move	#ZZ_CFG_ACK_VBL,(ZZ_REG_CONFIG,a2)
		ZZ_DBGL	'a'
		addq.l	#1,(zv_st_irqs,a3)
		bsr	zzi_service
		ZZ_DBGL	'e'
.fin		tst.b	(zv_finpend,a3)
		beq	.idle
		clr.b	(zv_finpend,a3)
		moveq	#WAUDIRQF_FINISHED,d7
.idle		tst.b	(zv_state,a3)		;nothing left to feed: the card
		bne	.end			;interrupt is not needed
		tst.b	(zv_armed,a3)
		beq	.end
		bsr	zzi_disarm
.end		move.l	d7,d0
		movem.l	(a7)+,d2-d7/a2-a3
		rts

;----------------------------------------
; feed the ring and publish the producer line
; called by zz9000_irq and by zz9000_play for the first part of a track
; IN:	a3 = APTR vars
; OUT:	-

zzi_service	movem.l	d2-d7/a2,-(a7)
		tst.b	(zv_dead,a3)
		bne	.out
		ZZ_DBGL	'c'
		bsr	zzi_credit
		tst.b	(zv_dead,a3)
		bne	.out
		tst.l	d1
		bne	.publish		;firmware line was updating
		move.l	d0,d2			;D2 = consumed cursor
		move.l	(zv_wrlo,a3),d3
		sub.l	d2,d3			;D3 = bytes written, not consumed
		moveq	#0,d0
		move.b	(zv_state,a3),d0
		beq	.publish		;ZST_IDLE
		tst.b	(zv_codecmode,a3)
		bne	.codec
		cmp.b	#ZST_FIN,d0
		beq	.fin
		bsr	zzi_budget		;D4 = frames
		cmp.b	#ZST_PAD,(zv_state,a3)
		beq	.pad

	;feed audio data
.play		tst.l	d4
		beq	.publish
		move.l	(zv_left,a3),d5
		bne	.havedata
		btst	#WHDLAUDB_LOOP,(zv_pflg+3,a3)
		beq	.endofdata
		move.l	(zv_start,a3),(zv_ptr,a3)	;loop, continue at the start
		move.l	(zv_total,a3),(zv_left,a3)
		bra	.play
.havedata	cmp.l	d4,d5
		bls	.n
		move.l	d4,d5
.n		sub.l	d5,d4
		moveq	#1,d7
		bsr	zzi_write
		bra	.play

	;end of the data, complete the last period with silence
.endofdata	bsr	zzi_calcpad
		move.b	#ZST_PAD,(zv_state,a3)

.pad		move.l	(zv_pad,a3),d5
		beq	.paddone
		lsr.l	#2,d5
		cmp.l	d4,d5
		bls	.pn
		move.l	d4,d5
.pn		tst.l	d5
		beq	.publish
		sub.l	d5,d4
		moveq	#0,d7
		bsr	zzi_write
		move.l	d5,d0
		lsl.l	#2,d0
		sub.l	d0,(zv_pad,a3)
		bne	.publish
.paddone	move.b	#ZST_FIN,(zv_state,a3)
		tst.l	(zv_seen,a3)
		bne	.publish		;firmware with flush waits for drain
		st	(zv_finpend,a3)
		bra	.publish

	;everything is written, pause the lease when the card played it all
.fin		tst.l	d3
		bne	.publish
		tst.l	(zv_seen,a3)
		beq	.paused			;old firmware: FINISHED already sent
		st	(zv_finpend,a3)
.paused
		move.l	#ZZ9K_RING_PAUSED,(zv_pflags,a3)
		clr.b	(zv_state,a3)
		bra	.publish

.codec		bsr	zzi_codec_service

.publish	ZZ_DBGL	'p'
		bsr	zzi_publish
.out		movem.l	(a7)+,d2-d7/a2
		rts

;----------------------------------------
; Feed a codec lease. d3 is the number of bytes still in the ring, based
; on actual credits even for a newly flushed stream: don't overwrite bytes
; the firmware may still be reading before it accepts the marker.

zzi_codec_service
		move.l	(zv_stream,a3),d0
		cmp.l	(zv_seen,a3),d0
		bne	.feed			;the firmware line describes an older stream
		cmp.l	#ZZ_DECODE_ERROR,(zv_decode,a3)
		beq	.error
		cmp.l	#ZZ_DECODE_DRAINED,(zv_decode,a3)
		beq	.finished
.feed		cmp.b	#ZST_FIN,(zv_state,a3)
		beq	.rts			;EOS published, now wait for the card
		;D6 = unused feed budget in frames or bytes
		tst.l	(zv_codec,a3)
		beq	.pcm

	;Compressed data is byte granular. Leave a few bytes free so that a
	;subsequent PCM start can be aligned without waiting for a credit.
		move.l	(zv_cap,a3),d6
		subq.l	#3,d6
		sub.l	d3,d6
		bls	.rts
		cmp.l	#ZZ_CALLMAX,d6
		bls	.bounded
		move.l	#ZZ_CALLMAX,d6
		bra	.bounded
.pcm		bsr	zzi_budget
		move.l	d4,d6
.bounded	move.l	(zv_left,a3),d5
		beq	.eof
		tst.l	d6
		beq	.rts
		cmp.l	d5,d6
		bhs	.write
		move.l	d6,d5
.write		sub.l	d5,d6
		tst.l	(zv_codec,a3)
		beq	.pcmwrite
		bsr	zzi_copybytes
		bra	.check
.pcmwrite	moveq	#1,d7
		bsr	zzi_write
.check		tst.l	(zv_left,a3)
		bne	.rts
.eof		btst	#WHDLAUDB_LOOP,(zv_pflg+3,a3)
		bne	.loop
		or.l	#ZZ_STREAM_END,(zv_streamflags,a3)
		move.b	#ZST_FIN,(zv_state,a3)
		rts
.loop		move.l	(zv_stream,a3),d0
		cmp.l	(zv_seen,a3),d0
		bne	.rts			;cannot replace an unlatched boundary
		move.l	(zv_codec,a3),d0
		moveq	#ZZ_STREAM_APPEND,d1
		bsr	zzi_marker
		move.l	(zv_start,a3),(zv_ptr,a3)
		move.l	(zv_total,a3),(zv_left,a3)
		bra	.bounded		;prime the repetition with the same budget
.rts		rts

.error		move.l	(zv_error,a3),(zv_last_error,a3)
.finished	move.l	#ZZ9K_RING_PAUSED,(zv_pflags,a3)
		clr.b	(zv_state,a3)
		st	(zv_finpend,a3)
		rts

;----------------------------------------
; Copy d5 compressed bytes, in at most two parts across the ring boundary.
; The ring is uncached: use long stores when both addresses are aligned.

zzi_copybytes	move.l	d5,d4
		move.l	(zv_ptr,a3),a0
.part		move.l	(zv_off,a3),d0
		move.l	(zv_ring,a3),a1
		add.l	d0,a1
		move.l	(zv_cap,a3),d2
		sub.l	d0,d2
		cmp.l	d4,d2
		bls	.have
		move.l	d4,d2
.have		move.l	d2,d3
		move.l	a0,d0
		move.l	a1,d1
		or.l	d1,d0
		and.l	#3,d0
		bne	.tail
		lsr.w	#2,d3
		bra	.lcount
.long		move.l	(a0)+,(a1)+
.lcount		dbf	d3,.long
		move.l	d2,d3
		and.w	#3,d3
.tail		bra	.bcount
.byte		move.b	(a0)+,(a1)+
.bcount		dbf	d3,.byte
		move.l	d2,d0
		bsr	zzi_advance
		sub.l	d2,d4
		bne	.part
		move.l	a0,(zv_ptr,a3)
		sub.l	d5,(zv_left,a3)
		rts

; Add d0 bytes to the ring position and the monotonic write cursor.
; d1 destroyed.
zzi_advance	move.l	(zv_off,a3),d1
		add.l	d0,d1
		cmp.l	(zv_cap,a3),d1
		blo	.off
		sub.l	(zv_cap,a3),d1
.off		move.l	d1,(zv_off,a3)
		add.l	d0,(zv_wrlo,a3)
		bcc	.rts
		addq.l	#1,(zv_wrhi,a3)
.rts		rts

;----------------------------------------
; frames to write: fill the ring up to ZZ_LEAD bytes in front of the
; consumed cursor, at most ZZ_CALLMAX bytes per call
; IN:	d2 = ULONG consumed cursor
;	d3 = ULONG bytes written and not consumed
; OUT:	d4 = ULONG frames, 0 if nothing is to be written
;	d0/d1 destroyed

zzi_budget	moveq	#0,d4
		move.l	(zv_cap,a3),d0
		cmp.l	#ZZ_LEAD,d0
		bls	.t
		move.l	#ZZ_LEAD,d0
.t		sub.l	d3,d0			;the lead is not above the capacity
		bls	.rts
		cmp.l	#ZZ_CALLMAX,d0
		bls	.m
		move.l	#ZZ_CALLMAX,d0
.m		lsr.l	#2,d0
		move.l	d0,d4
.rts		rts

;----------------------------------------
; bytes of silence after the last frame of a track
; the firmware converts whole 20 ms periods only, so the last period is
; completed, and a track which is shorter than the preroll of the firmware
; is extended until the card starts to play (nothing consumed yet)
; IN:	d2 = ULONG consumed cursor
; OUT:	zv_pad
;	d0-d1 destroyed

zzi_calcpad	moveq	#0,d0
		move.l	(zv_phase,a3),d1
		beq	.nophase
		move.l	#ZZ_PERIOD,d0
		sub.l	d1,d0
.nophase	tst.l	(zv_seen,a3)
		beq	.legacy
		move.l	d2,d1
		sub.l	(zv_startlo,a3),d1
		bgt	.set			;already consumed data from this stream
		bra	.written
.legacy		tst.l	d2
		bne	.set
.written	move.l	(zv_wrlo,a3),d1
		sub.l	(zv_startlo,a3),d1	;preroll must follow the new marker
		add.l	d0,d1
		cmp.l	#ZZ_PREROLL,d1
		bhs	.set
		neg.l	d1
		add.l	#ZZ_PREROLL+ZZ_PERIOD-1,d1	;missing bytes rounded up
		divu	#ZZ_PERIOD,d1
		and.l	#$ffff,d1
		mulu	#ZZ_PERIOD,d1
		add.l	d1,d0
.set		move.l	d0,(zv_pad,a3)
		rts

;----------------------------------------
; write frames into the ring
; IN:	a3 = APTR vars
;	d5 = ULONG frames, 1..64k
;	d7 = ULONG 0 = silence, <>0 = frames of the track
; OUT:	d0-d1 destroyed

zzi_write	movem.l	d2-d6/a0-a2,-(a7)
		move.l	d5,d4			;D4 = frames left to write
		lea	(zzi_kern_zero,pc),a2
		tst.l	d7
		beq	.loop
		bsr	zzi_pickkernel
.loop		move.l	(zv_cap,a3),d0
		sub.l	(zv_off,a3),d0
		lsr.l	#2,d0			;frames up to the end of the ring
		cmp.l	d4,d0
		bls	.n
		move.l	d4,d0
.n		move.l	d0,d5			;D5 = frames of this part
		move.l	(zv_ring,a3),a1
		add.l	(zv_off,a3),a1
		move.l	(zv_ptr,a3),a0
		move.l	(zv_vol,a3),d6
		move.l	d5,d3
		subq	#1,d3
		jsr	(a2)			;a0/a1 advance, d0-d2 destroyed
		move.l	a0,(zv_ptr,a3)
		move.l	d5,d0
		lsl.l	#2,d0			;bytes
		bsr	zzi_advance
		tst.b	(zv_codecmode,a3)
		bne	.data
		move.l	(zv_phase,a3),d1
		add.l	d0,d1
.phase		cmp.l	#ZZ_PERIOD,d1
		blo	.phased
		sub.l	#ZZ_PERIOD,d1
		bra	.phase
.phased		move.l	d1,(zv_phase,a3)
.data
		tst.l	d7
		beq	.nodata
		sub.l	d5,(zv_left,a3)
.nodata		sub.l	d5,d4
		bne	.loop
		movem.l	(a7)+,d2-d6/a0-a2
		rts

;----------------------------------------
; choose the routine which converts and copies frames, depends on the
; format of the track and the volume
; IN:	a3 = APTR vars
; OUT:	a2 = APTR routine
;	d0-d1 destroyed
;
; routines
; IN:	a0 = APTR frames of the track
;	a1 = APTR destination, long aligned
;	d3 = UWORD number of frames minus 1
;	d6 = ULONG volume, 0..$7fff
; OUT:	a0/a1 advanced
;	d0-d2 destroyed

zzi_pickkernel	moveq	#0,d1
		move.b	(zv_fmt,a3),d1
		bne	.notstereo16
		cmp.l	#$7fff,(zv_vol,a3)
		blo	.s16vol
		move.l	(zv_ptr,a3),d0
		btst	#1,d0
		bne	.copyw
		lea	(zzi_kern_copy,pc),a2
		rts
.copyw		lea	(zzi_kern_copyw,pc),a2
		rts
.s16vol		lea	(zzi_kern_s16s,pc),a2
		rts
.notstereo16	lea	(zzi_kern_s16m,pc),a2
		cmp.b	#1,d1
		beq	.rts
		lea	(zzi_kern_u8s,pc),a2
		cmp.b	#2,d1
		beq	.rts
		lea	(zzi_kern_u8m,pc),a2
.rts		rts

	;silence
zzi_kern_zero	moveq	#0,d0
.l		move.l	d0,(a1)+
		dbf	d3,.l
		rts

	;16 bit stereo at full volume, the track is long aligned
zzi_kern_copy	move.l	(a0)+,(a1)+
		dbf	d3,zzi_kern_copy
		rts

	;16 bit stereo at full volume, the track is word aligned
zzi_kern_copyw	move	(a0)+,d0
		swap	d0
		move	(a0)+,d0
		move.l	d0,(a1)+
		dbf	d3,zzi_kern_copyw
		rts

	;samples are little endian and unsigned for 8 bit; the volume is applied
	;as (sample*volume)>>15
	;SAMPLE s16 reads a sample from (a0)+ into d0.w as native word
	;SCALE multiplies d0.w, the result is a little endian word
ZZ_S16		MACRO
		move	(a0)+,\1
		ror	#8,\1
		ENDM
ZZ_U8		MACRO
		move.b	(a0)+,\1
		eor.b	#$80,\1
		lsl	#8,\1
		ENDM
ZZ_SCALE	MACRO
		muls	d6,\1
		add.l	\1,\1
		swap	\1
		ror	#8,\1
		ENDM

zzi_kern_s16s
.l		ZZ_S16	d0
		ZZ_SCALE d0
		ZZ_S16	d1
		ZZ_SCALE d1
		swap	d0
		move	d1,d0
		move.l	d0,(a1)+
		dbf	d3,.l
		rts

zzi_kern_s16m
.l		ZZ_S16	d0
		ZZ_SCALE d0
		move	d0,d1
		swap	d0
		move	d1,d0
		move.l	d0,(a1)+
		dbf	d3,.l
		rts

zzi_kern_u8s
.l		ZZ_U8	d0
		ZZ_SCALE d0
		ZZ_U8	d1
		ZZ_SCALE d1
		swap	d0
		move	d1,d0
		move.l	d0,(a1)+
		dbf	d3,.l
		rts

zzi_kern_u8m
.l		ZZ_U8	d0
		ZZ_SCALE d0
		move	d0,d1
		swap	d0
		move	d1,d0
		move.l	d0,(a1)+
		dbf	d3,.l
		rts

;----------------------------------------
; check a WAV file and set the track variables
; the file must be PCM, 8 or 16 bit, mono or stereo, 44100 Hz, the
; fmt chunk must come before the data chunk, chunks may be in any order
; IN:	a0 = APTR file, long aligned
;	d0 = ULONG length
; OUT:	d0 = LONG 0 = playable, -1 = not
;	a0-a1/d1 destroyed

zzi_parse	movem.l	d2-d7/a2,-(a7)
		move.l	a0,a2			;A2 = position
		move.l	a0,d5
		add.l	d0,d5			;D5 = end
		moveq	#0,d7			;D7 = fmt chunk seen
		cmp.l	#12,d0
		blo	.fail
		cmp.l	#$52494646,(a2)		;RIFF
		bne	.fail
		cmp.l	#$57415645,(8,a2)	;WAVE
		bne	.fail
		lea	(12,a2),a2
.chunk		move.l	d5,d0
		sub.l	a2,d0			;bytes left
		bcs	.fail
		cmp.l	#8,d0
		blo	.fail
		lea	(4,a2),a0
		bsr	zzi_le32
		move.l	d0,d3			;D3 = size of the chunk
		lea	(8,a2),a1		;A1 = body
		move.l	d5,d4
		sub.l	a1,d4			;D4 = bytes left behind the header
		cmp	#$666d,(a2)		;fmt
		bne	.notfmt
		cmp	#$7420,(2,a2)
		bne	.notfmt
		cmp.l	#16,d4
		blo	.fail
		cmp.l	#16,d3
		blo	.fail
		move.l	a1,a0
		bsr	zzi_le16
		cmp	#1,d0			;PCM
		bne	.fail
		lea	(2,a1),a0
		bsr	zzi_le16
		move.l	d0,d2			;D2 = channels
		cmp	#1,d2
		blo	.fail
		cmp	#2,d2
		bhi	.fail
		lea	(4,a1),a0
		bsr	zzi_le32
		cmp.l	#ZZ_SRCRATE,d0
		bne	.fail
		lea	(14,a1),a0
		bsr	zzi_le16
		moveq	#0,d6			;D6 = format
		cmp	#8,d0
		beq	.8bit
		cmp	#16,d0
		bne	.fail
		bra	.bits
.8bit		moveq	#2,d6
.bits		cmp	#1,d2
		bne	.stereo
		addq	#1,d6
.stereo		move.b	(.shifts,pc,d6.w),d2
		moveq	#1,d0
		lsl	d2,d0			;bytes of a frame
		move.l	d0,d1
		lea	(12,a1),a0
		bsr	zzi_le16
		cmp	d1,d0			;block align
		bne	.fail
		move.b	d6,(zv_fmt,a3)
		move.b	d2,(zv_shift,a3)
		moveq	#1,d7
		bra	.skip

.notfmt		cmp	#$6461,(a2)		;data
		bne	.skip
		cmp	#$7461,(2,a2)
		bne	.skip
		tst.l	d7
		beq	.fail
		cmp.l	d4,d3
		bls	.size
		move.l	d4,d3			;streamed file, size is wrong
.size		move.l	a1,d0
		btst	#0,d0
		bne	.fail
		moveq	#0,d0
		move.b	(zv_shift,a3),d0
		lsr.l	d0,d3			;frames
		beq	.fail
		move.l	d3,(zv_left,a3)
		move.l	d3,(zv_total,a3)
		move.l	a1,(zv_start,a3)
		move.l	a1,(zv_ptr,a3)
		moveq	#0,d0
		bra	.end

	;next chunk, chunks are padded to an even size
.skip		cmp.l	d4,d3
		bhi	.fail
		move.l	d3,d0
		addq.l	#1,d0
		bclr	#0,d0
		lea	(8,a2),a2
		add.l	d0,a2
		bra	.chunk

.fail		moveq	#-1,d0
.end		movem.l	(a7)+,d2-d7/a2
		rts

.shifts		dc.b	2,1,1,0

;----------------------------------------
; read little endian values, byte by byte
; IN:	a0 = APTR value
; OUT:	d0 = ULONG value

zzi_le32	moveq	#0,d0
		move.b	(3,a0),d0
		lsl.l	#8,d0
		move.b	(2,a0),d0
		lsl.l	#8,d0
		move.b	(1,a0),d0
		lsl.l	#8,d0
		move.b	(a0),d0
		rts

zzi_le16	moveq	#0,d0
		move.b	(1,a0),d0
		lsl	#8,d0
		move.b	(a0),d0
		rts

;----------------------------------------
; read a big endian long byte by byte, the address may be odd
; IN:	a0 = APTR value
; OUT:	d0 = ULONG value

zzi_be32	moveq	#0,d0
		move.b	(a0),d0
		lsl.l	#8,d0
		move.b	(1,a0),d0
		lsl.l	#8,d0
		move.b	(2,a0),d0
		lsl.l	#8,d0
		move.b	(3,a0),d0
		rts

;----------------------------------------
; find out what a track file is, by its first bytes and not by its name
; IN:	a0 = APTR file
;	d0 = ULONG length
; OUT:	d0 = LONG codec, ZCODEC_WAV, ZCODEC_MP3, ZCODEC_FLAC or ZCODEC_VORBIS;
;	   or -ZRS_* when the file is not played
;	a1 = APTR first byte to give to the decoder: behind ID3v2 tags and, for
;	   MPEG audio, at the first frame header
;	d1 destroyed

zzi_detect	movem.l	d2-d4/a2,-(a7)
		move.l	a0,a2			;A2 = position
		move.l	d0,d2			;D2 = bytes left
		move.l	d2,d4			;original file length
		move.l	a0,a1
		cmp.l	#12,d2
		blo	.id3
		bsr	zzi_be32
		cmp.l	#$52494646,d0		;RIFF
		bne	.id3
		lea	(8,a2),a0
		bsr	zzi_be32
		cmp.l	#$57415645,d0		;WAVE
		bne	.unknown
		moveq	#ZCODEC_WAV,d0
		bra	.ret

	;ID3v2 tags: "ID3", version, revision, flags, size as four 7 bit bytes
	;(the size does not count the header, nor the footer if flag 4 is set)
.id3		cmp.l	#10,d2
		blo	.magic
		move.l	a2,a0
		bsr	zzi_be32
		and.l	#$ffffff00,d0
		cmp.l	#$49443300,d0		;"ID3"
		bne	.magic
		cmp.b	#4,(3,a2)		;ID3v2.2 through 2.4
		bhi	.unknown
		cmp.b	#2,(3,a2)
		blo	.unknown
		moveq	#0,d1
		moveq	#3,d3
		lea	(6,a2),a0
.size		move.b	(a0)+,d0
		bmi	.unknown
		lsl.l	#7,d1
		or.b	d0,d1
		dbf	d3,.size
		add.l	#10,d1
		cmp.b	#4,(3,a2)		;only v2.4 defines a footer
		bne	.nofooter
		btst	#4,(5,a2)
		beq	.nofooter
		add.l	#10,d1
.nofooter	cmp.l	d2,d1
		bhs	.unknown		;the tag is the whole file
		add.l	d1,a2
		sub.l	d1,d2
		;Do not scan arbitrary payload or an unbounded chain of tags.

.magic		move.l	a2,a1
		cmp.l	#4,d2
		blo	.unknown
		move.l	a2,a0
		bsr	zzi_be32
		cmp.l	#$664c6143,d0		;"fLaC"
		beq	.flac
		cmp.l	#$4f676753,d0		;"OggS"
		beq	.ogg
		cmp.l	#$3026b275,d0		;ASF header GUID, wma
		beq	.asf
		cmp.l	#12,d2
		blo	.mpeg
		lea	(4,a2),a0
		bsr	zzi_be32
		cmp.l	#$66747970,d0		;"ftyp", mp4 and m4a
		beq	.mp4

	;MPEG audio: a frame header is $ffe, version, layer, protection,
	;bitrate index, sample rate index; the first frame may follow some
	;padding behind a tag. ADTS (aac) has layer 0 in the same place.
.mpeg		cmp.b	#$ff,(a2)
		bne	.scan0
		move.b	(1,a2),d0
		and.b	#$f6,d0
		cmp.b	#$f0,d0
		beq	.aac
.scan0		;Allow only zero padding after a verified ID3 tag.
		cmp.l	d2,d4
		beq	.try
		move.l	d2,d3
		cmp.l	#ZZ_MPEGSCAN,d3
		bls	.padsz
		move.l	#ZZ_MPEGSCAN,d3
.padsz		subq.l	#4,d3
		bra	.padcount
.pad0		tst.b	(a1)
		bne	.try
		addq.l	#1,a1
.padcount	dbf	d3,.pad0
		bra	.unknown
.try		cmp.b	#$ff,(a1)
		bne	.unknown
		move.b	(1,a1),d0
		move.b	d0,d1
		and.b	#$e0,d1
		cmp.b	#$e0,d1			;sync
		bne	.unknown
		move.b	d0,d1
		lsr.b	#3,d1
		and.b	#3,d1
		cmp.b	#1,d1			;version 1 is reserved
		beq	.unknown
		and.b	#6,d0
		cmp.b	#2,d0			;Layer III (MP3)
		bne	.unknown
		move.b	(2,a1),d0
		lsr.b	#4,d0
		cmp.b	#15,d0			;bitrate index 15 is invalid
		beq	.unknown
		move.b	(2,a1),d0
		and.b	#$0c,d0
		cmp.b	#$0c,d0			;sample rate index 3 is invalid
		beq	.unknown
		moveq	#ZCODEC_MP3,d0
		bra	.ret

.flac		moveq	#ZCODEC_FLAC,d0
		bra	.ret

	;an Ogg page: "OggS", version, flags, granule (8), serial (4), page
	;number (4), checksum (4), segment count, segment table; the first
	;packet of the first page names the codec
.ogg		cmp.l	#28,d2
		blo	.unknown
		tst.b	(4,a2)			;revision 0
		bne	.unknown
		cmp.b	#8,(27,a2)		;first segment can hold the ID
		blo	.unknown
		moveq	#0,d3
		move.b	(26,a2),d3		;segments
		beq	.unknown
		add.w	#27,d3
		moveq	#8,d1
		add.l	d3,d1			;header and the first 8 bytes
		cmp.l	d2,d1
		bhi	.unknown
		lea	(a2,d3.w),a0		;first packet
		bsr	zzi_be32
		cmp.l	#$4f707573,d0		;"Opus"
		beq	.opus
		cmp.l	#$01766f72,d0		;1, "vor"
		bne	.oggother
		lea	(a2,d3.w),a0
		cmp.b	#'b',(4,a0)		;"bis"
		bne	.oggother
		cmp.b	#'i',(5,a0)
		bne	.oggother
		cmp.b	#'s',(6,a0)
		bne	.oggother
		moveq	#ZCODEC_VORBIS,d0
		bra	.ret
.opus
		lea	(4,a2,d3.w),a0
		bsr	zzi_be32
		cmp.l	#$48656164,d0		;"Head"
		bne	.oggother
		moveq	#-ZRS_OPUS,d0
		bra	.ret
.oggother	moveq	#-ZRS_OGG,d0
		bra	.ret

.asf		moveq	#-ZRS_ASF,d0
		bra	.ret
.mp4		moveq	#-ZRS_MP4,d0
		bra	.ret
.aac		moveq	#-ZRS_AAC,d0
		bra	.ret
.unknown	moveq	#-ZRS_UNKNOWN,d0
.ret		movem.l	(a7)+,d2-d4/a2
		rts

;----------------------------------------
	IFD ZZ9000_DIAG
; progress codes
; IN:	a3 = APTR vars
;	d0 = character
; OUT:	all registers and the condition codes preserved

zzi_dbg		movem.l	d0/a0,-(a7)
		move.l	(zv_hw,a3),d0
		beq	.e
		move.l	d0,a0
		move.l	(a7),d0
		move	d0,(ZZ_REG_PRINT,a0)
.e		movem.l	(a7)+,d0/a0
		rts

	;the same, but only while zv_dbgcnt lasts
zzi_dbgl	tst.l	(zv_dbgcnt,a3)
		beq	.e
		subq.l	#1,(zv_dbgcnt,a3)
		bsr	zzi_dbg
.e		rts

	;d0.w as four hex digits and a blank, limited
zzi_dbghexl	tst.l	(zv_dbgcnt,a3)
		beq	.e
		subq.l	#1,(zv_dbgcnt,a3)
		movem.l	d0-d2,-(a7)
		move	d0,d2
		moveq	#3,d1
.l		rol	#4,d2
		move	d2,d0
		and	#15,d0
		add	#'0',d0
		cmp	#'9',d0
		bls	.d
		addq	#7,d0
.d		bsr	zzi_dbg
		dbf	d1,.l
		moveq	#' ',d0
		bsr	zzi_dbg
		movem.l	(a7)+,d0-d2
.e		rts

	;ENV:ZZ9K_WA_NORESUME, _NOIRQ, _NOARM switch parts off
zzi_envflags	movem.l	d0-d1/a0-a1,-(a7)
		move.l	#300,(zv_dbgcnt,a3)
		lea	(zz9000_e_noresume,pc),a0
		bsr	zzi_envflag
		tst.l	d0
		beq	.1
		bset	#ZZ_DIAG_NORESUME,(zv_diag,a3)
.1		lea	(zz9000_e_noirq,pc),a0
		bsr	zzi_envflag
		tst.l	d0
		beq	.3
		bset	#ZZ_DIAG_NOIRQ,(zv_diag,a3)
.3		lea	(zz9000_e_noarm,pc),a0
		bsr	zzi_envflag
		tst.l	d0
		beq	.4
		bset	#ZZ_DIAG_NOARM,(zv_diag,a3)
.4		lea	(zz9000_t_diag,pc),a0
		moveq	#0,d0
		move.b	(zv_diag,a3),d0
		move.l	d0,-(a7)
		move.l	a7,a1
		bsr	_trace
		addq.l	#4,a7
		movem.l	(a7)+,d0-d1/a0-a1
		rts

	;IN a0 = name, a2 = exec; OUT d0 = 1 if the file exists
zzi_envflag	movem.l	d2-d4/a6,-(a7)
		move.l	a0,d4
		move.l	a2,a6
		lea	(zz9000_n_dos,pc),a1
		moveq	#0,d0
		jsr	(_LVOOpenLibrary,a6)
		move.l	d0,d3
		beq	.end
		move.l	d0,a6
		move.l	d4,d1
		move.l	#MODE_OLDFILE,d2
		jsr	(_LVOOpen,a6)
		move.l	d0,d1
		beq	.no
		jsr	(_LVOClose,a6)
		moveq	#1,d4
		bra	.close
.no		moveq	#0,d4
.close		move.l	d3,a1
		move.l	a2,a6
		jsr	(_LVOCloseLibrary,a6)
		move.l	d4,d0
.end		movem.l	(a7)+,d2-d4/a6
		rts

zz9000_e_noresume	dc.b	"ENV:ZZ9K_WA_NORESUME",0
zz9000_e_noirq		dc.b	"ENV:ZZ9K_WA_NOIRQ",0
zz9000_e_noarm		dc.b	"ENV:ZZ9K_WA_NOARM",0
zz9000_t_diag		dc.b	"audio: ZZ9000AX: diagnostic build, switches %ld",0
	EVEN
	ENDC

zz9000_name		dc.b	"ZZ9000AX",0
zz9000_exts		dc.b	"wav",0,0
zz9000_codecexts	dc.b	"flac",0,"wav",0,"mp3",0,"ogg",0,0
zz9000_n_expansion	dc.b	"expansion.library",0
zz9000_n_dos		dc.b	"dos.library",0
zz9000_n_zz9k		dc.b	"zz9k.library",0
zz9000_n_int2env	dc.b	"ENV:ZZ9K_INT2",0
zz9000_s_intact		dc.b	"intact",0
zz9000_s_lost		dc.b	"lost",0
zz9000_t_noexp		dc.b	"audio: ZZ9000AX: no expansion.library",0
zz9000_t_nocard		dc.b	"audio: ZZ9000AX: no ZZ9000 found",0
zz9000_t_z2		dc.b	"audio: ZZ9000AX: Zorro II is not supported",0
zz9000_t_noint2		dc.b	"audio: ZZ9000AX: INT2 mode is required, set ENV:ZZ9K_INT2 or int2 = on in ZZ9000.CFG",0
zz9000_t_nolib		dc.b	"audio: ZZ9000AX: no zz9k.library 2.44",0
zz9000_t_oldlib		dc.b	"audio: ZZ9000AX: zz9k.library 2.%ld is too old, 2.44 is required",0
zz9000_t_nofabric	dc.b	"audio: ZZ9000AX: the firmware has no audio fabric",0
zz9000_t_norate		dc.b	"audio: ZZ9000AX: the audio fabric has no source rate leases",0
zz9000_t_busy		dc.b	"audio: ZZ9000AX: no free direct ring lease, status %ld",0
zz9000_t_noidle		dc.b	"audio: ZZ9000AX: the firmware refuses a lease with IDLE_PAUSED (status %ld), update the firmware",0
zz9000_t_badgrant	dc.b	"audio: ZZ9000AX: unusable lease: slot %ld generation %ld ring $%lx capacity %ld control $%lx",0
zz9000_t_ok		dc.b	"audio: ZZ9000AX: lease slot %ld generation %ld ring capacity %ld at $%lx, control at $%lx",0
zz9000_t_stat		dc.b	"audio: ZZ9000AX: tracks %ld, refused %ld, interrupts %ld, suspend %ld, resume %ld, lease %s (firmware status %ld)",0
zz9000_t_refused	dc.b	"audio: ZZ9000AX: last track refusal: %s",0
zz9000_t_decode		dc.b	"audio: ZZ9000AX: last codec decode error %ld",0
	EVEN
zz9000_refusals
		dc.w	.unknown-zz9000_refusals,.opus-zz9000_refusals
		dc.w	.ogg-zz9000_refusals,.asf-zz9000_refusals
		dc.w	.mp4-zz9000_refusals,.aac-zz9000_refusals
		dc.w	.wav-zz9000_refusals,.codec-zz9000_refusals
		dc.w	.lease-zz9000_refusals
.unknown	dc.b	"unknown file format",0
.opus		dc.b	"Ogg Opus is not supported",0
.ogg		dc.b	"Ogg file does not contain Vorbis",0
.asf		dc.b	"ASF/WMA is not supported",0
.mp4		dc.b	"MP4/M4A is not supported",0
.aac		dc.b	"AAC is not supported",0
.wav		dc.b	"invalid or unsupported WAV",0
.codec		dc.b	"firmware has no codec ring",0
.lease		dc.b	"lease lost",0
	EVEN
