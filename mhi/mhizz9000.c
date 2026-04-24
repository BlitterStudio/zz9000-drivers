/*
 * MNT ZZ9000AX Amiga MHI driver (Hardware Accelerated)
 *
 * Copyright (C) 2022, Thomas Wenzel
 * Copyright (C) 2022, Lucie L. Hartmann <lucie@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * Hardened by Dimitris Panokostas <midwan@gmail.com> (2026)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#include <libraries/mhi.h>

#include <clib/debug_protos.h>
#include <clib/alib_protos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include <hardware/intbits.h>

#include "mhizz9000.h"
#include "ax.h"

#define OPTIMIZED_TRANSFER

// Comment out to enable debug output:
#define KPrintF(...)

#define DEVF_INT2MODE 1

#define ZZ_BYTES_PER_PERIOD 3840
#define AUDIO_BUFSZ ZZ_BYTES_PER_PERIOD*8 // TODO: query from hardware

#define REG_ZZ_CONFIG        0x04
#define REG_ZZ_AUDIO_SWAB    0x70
#define REG_ZZ_DECODER_FIFO  0x72
#define REG_ZZ_AUDIO_SCALE   0x74
#define REG_ZZ_AUDIO_PARAM   0x76
#define REG_ZZ_AUDIO_VAL     0x78
#define REG_ZZ_DECODER_PARAM 0x7A
#define REG_ZZ_DECODER_VAL   0x7C
#define REG_ZZ_DECODE        0x7E
#define REG_ZZ_AUDIO_CONFIG  0xF4

#define ID3V2_HEADER_LENGTH 10

#define FIFOSIZE (1152*4)
//#define FIFOSIZE (16*1024+1)

typedef enum {
	DECODE_CLEAR,
	DECODE_INIT,
	DECODE_RUN
} DECODE_COMMAND;

#define BSWAP_S(x) ((UWORD) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define BSWAP_L(x) (((((ULONG)x) & 0xff000000u) >> 24) | ((((ULONG)x) & 0x00ff0000u) >> 8) | ((((ULONG)x) & 0x0000ff00u) << 8) | ((((ULONG)x) & 0x000000ffu) << 24))
#define BSWAP_P(x) (void*)(((((ULONG)x) & 0xff000000u) >> 24) | ((((ULONG)x) & 0x00ff0000u) >> 8) | ((((ULONG)x) & 0x0000ff00u) << 8) | ((((ULONG)x) & 0x000000ffu) << 24))


/* ******************************** */
/*  BEGIN ZZ9000AX parameter access */
/*  Don't worry!                    */
/*  The compiler inlines these!     */
/* ******************************** */
static void setRegister(struct MhiPlayer *mp, ULONG Register, UWORD Value) {
	*((volatile UWORD*)(mp->hw_addr+Register)) = Value;
}

static UWORD getRegister(struct MhiPlayer *mp, ULONG Register) {
	return *((volatile UWORD*)(mp->hw_addr+Register));
}

static void setAudioParam(struct MhiPlayer *mp, ULONG Param, UWORD Value) {
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = Param;
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL))	 = Value;
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;	
}

static void setDecoderParam(struct MhiPlayer *mp, ULONG Param, UWORD Value) {
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = Param;
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL))   = Value;
}

// Read an optional user override for AP_DSP_SET_VOLUMES from
// ENV:ZZ9K_MIX_LEVELS. The file (created e.g. with
// `setenv ZZ9K_MIX_LEVELS C040`) contains a 1-4 digit hex string packing
// ZZ9000AX/MHI output level in the high byte and Paula pass-through in
// the low byte. "0x" prefix, leading whitespace and trailing newlines
// are tolerated. Returns `default_value` on any failure so a stale or
// mangled file can never brick audio.
static UWORD read_mix_levels_env(UWORD default_value) {
	BPTR fh;
	UBYTE buf[16];
	LONG len;
	UWORD out = 0;
	int digits = 0;
	int i;

	if (!DOSBase) return default_value;
	fh = Open((CONST_STRPTR)"ENV:ZZ9K_MIX_LEVELS", MODE_OLDFILE);
	if (!fh) return default_value;
	len = Read(fh, buf, sizeof(buf) - 1);
	Close(fh);
	if (len <= 0) return default_value;
	buf[len] = 0;

	for (i = 0; i < len && digits < 4; i++) {
		UBYTE c = buf[i];
		int d;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (digits > 0) break;
			continue;
		}
		if (digits == 0 && c == '0' && (i + 1 < len) &&
		    (buf[i + 1] == 'x' || buf[i + 1] == 'X')) {
			i++;
			continue;
		}
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		out = (UWORD)((out << 4) | d);
		digits++;
	}
	return (digits > 0) ? out : default_value;
}
/* ****************************** */
/*  END ZZ9000AX parameter access */
/* ****************************** */

/* ************ */
/*  BEGIN FIFO  */
/* ************ */
// Clear FIFO on both sides.
static void clearFifo(struct MhiPlayer *mp) {
	mp->FifoMode = FIFO_PREFILL;
	mp->FifoWriteIdx = 0;
	// ZZ_DECODE (clear)
	setRegister(mp, REG_ZZ_DECODE, DECODE_CLEAR);
}

static void fillFifo(struct MhiPlayer *mp) {
	UBYTE *Buffer = (UBYTE *)mp->mp3_addr;
	LONG Space = 0;
	UWORD FifoReadIdx;
	struct ListNode *BufferNode;
	LONG i;

	// 1. Get FIFO Read Index from ZZ9k (we are the slave).
	FifoReadIdx = getRegister(mp, REG_ZZ_DECODER_FIFO);
	if(mp->FifoWriteIdx >= FifoReadIdx) {
		Space = FIFOSIZE-(mp->FifoWriteIdx-FifoReadIdx);
	}
	else {
		Space = FifoReadIdx-mp->FifoWriteIdx;
	}

	// 2. Calculate space left in FIFO.
	// In prefill mode fill the FIFO completely.
	if(mp->FifoMode == FIFO_PREFILL) {
		Space -= 1; // Note: Fill level limited for technical rasons.
	}
	// In operational mode fill it only half way to leave data for seeking back.
	else {
		Space -= FIFOSIZE/2;
	}
	if(Space <= 0) return;

	// 3. Fill the FIFO
	// Find first node in list that has not been completely played.
	for(BufferNode = (struct ListNode *)mp->BufferList->mlh_Head; BufferNode->Header.mln_Succ; BufferNode = (struct ListNode *)BufferNode->Header.mln_Succ) {
		if(BufferNode->Played == FALSE) {
			LONG BytesToCopy = BufferNode->Size - BufferNode->Index;
			if(BytesToCopy > Space) BytesToCopy = Space;

			#ifdef OPTIMIZED_TRANSFER
			// 3.1 Copy single bytes until we reach a 32-bit aligned destination address.
			if(BytesToCopy >= 3) {
				for(i=0; i<3; i++) {
					if((mp->FifoWriteIdx & 3) == 0) break;
					if(Space) {
						Buffer[mp->FifoWriteIdx++] = BufferNode->Buffer[BufferNode->Index++];
						if(mp->FifoWriteIdx >= FIFOSIZE) mp->FifoWriteIdx = 0;
						Space--;
						BytesToCopy--;
					}
				}
			}

			// 3.2 Optimized longword copy routine.
			LONG LongsToCopy = BytesToCopy/4;
			ULONG *src = (ULONG*)&BufferNode->Buffer[BufferNode->Index];
			ULONG *dst = (ULONG*)&Buffer[mp->FifoWriteIdx];
			for(i=0; i<LongsToCopy; i++) {
				*dst++ = *src++;
				mp->FifoWriteIdx  += 4;
				if(mp->FifoWriteIdx >= FIFOSIZE) {
					mp->FifoWriteIdx = 0;
					dst = (ULONG*)Buffer;
				}
			}
			Space             -= 4*LongsToCopy;
			BytesToCopy       -= 4*LongsToCopy;
			BufferNode->Index += 4*LongsToCopy;

			// 3.3 Copy remainder.
			#endif
			for(i=0; i<BytesToCopy; i++) {
				if(Space) {
					Buffer[mp->FifoWriteIdx++] = BufferNode->Buffer[BufferNode->Index++];
					if(mp->FifoWriteIdx >= FIFOSIZE) mp->FifoWriteIdx = 0;
					Space--;
				}
			}

			// If we have reached the end of the current buffer then...
			if(BufferNode->Index >= BufferNode->Size) {
				// ... mark this buffer as 'played'.
				BufferNode->Played = TRUE;	
				// ... signal the calling task that a buffer has been played.
				Signal(mp->MhiTask, mp->MhiMask);
			}
			break;
		}
	}

	mp->FifoMode = FIFO_OPERATIONAL;

	// 4. Set FIFO Write Index in ZZ9k (we are the master).
	setRegister(mp, REG_ZZ_DECODER_FIFO, mp->FifoWriteIdx);
}
/* ********** */
/*  END FIFO  */
/* ********** */

// Check whether AHI has its ISR installed on our shared interrupt level.
// MUST be called with Forbid() already active so the caller can combine
// this check with AddIntServer() into a single atomic claim step --
// otherwise AHI and MHI can both win the check and then both attach.
// The IntVects[] server list can be mutated by any task, so walking it
// unprotected would be unsafe in its own right.
static BOOL ahi_present_locked(struct MHI_LibBase *MhiLibBase) {
	struct List *IrqList;
	if(MhiLibBase->flags & DEVF_INT2MODE) {
		IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
	}
	else {
		IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
	}
	return FindName(IrqList, "ZZ9000AX") ? TRUE : FALSE;
}

BOOL UserLibInit(struct MHI_LibBase *MhiLibBase) {
	// Must start at NULL: FindConfigDev(NULL, ...) means "search from head".
	// Leaving cd uninitialized here is UB and, if BSS happens to hold a
	// non-NULL value across AutoInit reloads, FindConfigDev will skip past
	// or walk into invalid entries.
	struct ConfigDev* cd = NULL;
	ULONG hw_addr = 0;
	ULONG hw_size = 0;
	int ax_present;

	MhiLibBase->zorro_version = 0;
	MhiLibBase->hw_addr = 0;
	MhiLibBase->hw_size = 0;
	MhiLibBase->flags = 0;
	MhiLibBase->NumAllocatedDecoders = 0;

	ExpansionBase = (struct ExpansionBase*) OpenLibrary((STRPTR)"expansion.library", 0);
	if(!ExpansionBase) {
		KPrintF("Error: Can't open expansion.library.\n");
		return FALSE;
	}

	// Find Z2 or Z3 model of MNT ZZ9000.
	if((cd = (struct ConfigDev*)FindConfigDev(NULL, 0x6D6E, 0x4))) {
		MhiLibBase->zorro_version = 3;
		KPrintF("ZZ9000 Zorro 3 Version detected.\n");
	}
	else if((cd = (struct ConfigDev*)FindConfigDev(NULL, 0x6D6E, 0x3))) {
		MhiLibBase->zorro_version = 2;
		KPrintF("ZZ9000 Zorro 2 Version detected.\n");
	}

	if(!cd || MhiLibBase->zorro_version == 0) {
		KPrintF("Error: ZZ9000 not detected.\n");
		CloseLibrary((struct Library*)ExpansionBase);
		return FALSE;
	}

	// Read BoardAddr/Size while expansion.library is still open -- the
	// ConfigDev node is owned by that library and must not be dereferenced
	// after CloseLibrary, even if in practice expansion.library never
	// expunges.
	hw_addr = (ULONG)cd->cd_BoardAddr;
	hw_size = (ULONG)cd->cd_BoardSize;
	CloseLibrary((struct Library*)ExpansionBase);

	// REG_ZZ_AUDIO_CONFIG bit 0 is the "AX present" strap; mask explicitly
	// so other status bits can't ever make detection look successful.
	ax_present = (*((volatile UWORD*)(hw_addr+REG_ZZ_AUDIO_CONFIG))) & 1;
	if(!ax_present) {
		KPrintF("Error: ZZ9000AX not detected.\n");
		return FALSE;
	}

	KPrintF("HwAddr=0x%08lX, HwSize=0x%08lX\n", hw_addr, hw_size);
	MhiLibBase->hw_addr = hw_addr;
	MhiLibBase->hw_size = hw_size;

	BPTR fh;
	if((fh=Open((CONST_STRPTR)"ENV:ZZ9K_INT2",MODE_OLDFILE))) {
		Close(fh);
		MhiLibBase->flags |= DEVF_INT2MODE;
	}

	return TRUE;
}

void UserLibCleanup(struct MHI_LibBase *MhiLibBase) {
	// Nothing to clean up here because UserLibInit() didn't leave anything open.
}

extern ULONG dev_sisr(struct MhiPlayer *mp asm("a1"));
ULONG cdev_sisr(struct MhiPlayer *mp asm("a1")) {
	ULONG buf_samples = ZZ_BYTES_PER_PERIOD/4;
	// Bail if the decoder is no longer playing. A hard ISR that fired
	// between disable_hw_audio() and RemIntServer() can leave a soft IRQ
	// queued on Exec's SoftInts list; when Stop/Free subsequently set
	// Status != PLAYING under Forbid, the dispatched soft IRQ must be a
	// no-op so it cannot clobber buf_offset/FifoWriteIdx/FifoMode or
	// walk a freed BufferList.
	if(mp->Status != MHIF_PLAYING) return 0;
	fillFifo(mp);

	setRegister(mp, REG_ZZ_AUDIO_SCALE, buf_samples);

	setDecoderParam(mp, 4, (mp->decode_offset+mp->buf_offset)>>16);
	setDecoderParam(mp, 5, (mp->decode_offset+mp->buf_offset)&0xffff);
	setRegister(mp, REG_ZZ_DECODE, DECODE_RUN);
	
	// play buffer
	setRegister(mp, REG_ZZ_AUDIO_SWAB, (1<<15) | (mp->buf_offset >> 8)); // no byteswap, offset/256
	int overrun = getRegister(mp, REG_ZZ_AUDIO_SWAB);
	
	if (overrun == 1) {
	  mp->buf_offset = 0;
	} else {
	  mp->buf_offset += ZZ_BYTES_PER_PERIOD;
	}
	
	if (mp->buf_offset >= AUDIO_BUFSZ) {
	  mp->buf_offset = 0;
	}
	return 0;
}

extern ULONG dev_isr(struct MhiPlayer *mp asm("a1"));
ULONG cdev_isr(struct MhiPlayer *mp asm("a1")) {
	UWORD status = *(UWORD*)(mp->hw_addr+REG_ZZ_CONFIG);

	// audio interrupt signal set? (REG_ZZ_CONFIG bit 1, mask 0x02)
	if(status & 2) {
		// Ack/clear audio interrupt.
		*(USHORT*)(mp->hw_addr+REG_ZZ_CONFIG) = 8|32;
		// Cause soft interrupt to do the rest.
		Cause(&mp->sirq);
	}
	return 0;
}

// Initialise the Interrupt server nodes in mp so they are ready to be
// added or Cause()d. Split out from the install step so AllocDecoder can
// perform an atomic "check AHI is absent AND install our ISR" under a
// single Forbid() window.
static void prepare_irq_structs(struct MhiPlayer *mp) {
	// Software interrupts have only five allowable priority levels:
	// -32, -16, 0, +16, and +32
	mp->sirq.is_Node.ln_Pri  = 0;
	mp->sirq.is_Node.ln_Type = NT_INTERRUPT;
	mp->sirq.is_Node.ln_Name = "mhizz9000s";
	mp->sirq.is_Data = mp;
	mp->sirq.is_Code = (void*)dev_sisr;

	mp->irq.is_Node.ln_Type = NT_INTERRUPT;
	mp->irq.is_Node.ln_Pri  = 126; // High priority: this ISR must react quickly.
	mp->irq.is_Node.ln_Name = "mhizz9000";
	mp->irq.is_Data = mp;
	mp->irq.is_Code = (void*)dev_isr;
}

// Install the hard interrupt server. MUST be called under Forbid() so the
// caller can combine it with ahi_present_locked() atomically.
static void install_irq_server_locked(struct MhiPlayer *mp) {
	if (mp->flags & DEVF_INT2MODE) {
		AddIntServer(INTB_PORTS, &mp->irq);
	} else {
		AddIntServer(INTB_EXTER, &mp->irq);
	}
}

// Flip the HW-side audio interrupt bit. Called only when the decoder is
// actually playing; the ISR is otherwise a no-op because dev_isr gates on
// REG_ZZ_CONFIG bit 1 (mask 0x02) which the hardware only sets while running.
static void enable_hw_audio(struct MhiPlayer *mp) {
	setRegister(mp, REG_ZZ_AUDIO_CONFIG, 1);
}

static void disable_hw_audio(struct MhiPlayer *mp) {
	setRegister(mp, REG_ZZ_AUDIO_CONFIG, 0);
}

/*
 *
 */
APTR i_MHIAllocDecoder(REGA0(struct Task *mhi_task), REGD0(ULONG mhi_sigmask), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = NULL;

	mp = AllocVec(sizeof(struct MhiPlayer), MEMF_CLEAR);
	if(!mp) {
		KPrintF("Can't allocate MhiPlayer.\n");
		return NULL;
	}

	mp->hw_addr = MHI_LibBase->hw_addr;

	if (MHI_LibBase->zorro_version == 3) {
		// FIFO offset as in axmp3 (this is still within Zorro3 address range).
		mp->encoded_offset =  0x06000000;
		// Decoded audio offset right after FIFO with a little padding to be cache line aligned.
		mp->decode_offset  = (0x06000000 + FIFOSIZE + 32) & 0xFFFFFFE0;
	} else {
		// FIFO offset like offset_tx in AHI driver (this is still within Zorro2 address range).
		mp->encoded_offset = MHI_LibBase->hw_size - 0x20000;
		// Decoded audio offset at 96MB.
		mp->decode_offset  = 0x06000000;
	}
	KPrintF("encoded_offset = 0x%08lX\n", mp->encoded_offset);
	KPrintF("decode_offset  = 0x%08lX\n", mp->decode_offset);

	mp->mp3_addr     = MHI_LibBase->hw_addr + 0x10000 + mp->encoded_offset;
	mp->flags        = MHI_LibBase->flags;

	mp->decode_chunk_sz = 1920; // 16 bit sample pairs

	mp->MhiTask      = mhi_task;
	mp->MhiMask      = mhi_sigmask;
	mp->Status       = MHIF_STOPPED;

	mp->FifoMode     = FIFO_PREFILL;
	mp->FifoWriteIdx = 0;
	mp->buf_offset   = 0;
	mp->volume       = 100;
	mp->panning      = 50;

	mp->BufferList = AllocVec(sizeof(struct MinList), MEMF_PUBLIC|MEMF_CLEAR);
	if(!mp->BufferList) {
		FreeVec(mp);
		return NULL;
	}
	NewList((struct List *)mp->BufferList);

	// Populate the Interrupt nodes so the atomic-claim step below can
	// AddIntServer our hard ISR directly.
	prepare_irq_structs(mp);

	// Atomic ownership claim: in one Forbid() window, verify (a) no other
	// MHI decoder is already allocated and (b) AHI hasn't installed its
	// ISR, then install our own hard-IRQ server. Doing all three under one
	// Forbid closes the TOCTOU that would otherwise let AHI and MHI both
	// decide the card is free and then both attach. The HW audio bit stays
	// off until i_MHIPlay() fires, so the newly-installed ISR is a no-op
	// in the meantime.
	Forbid();
	if(MHI_LibBase->NumAllocatedDecoders) {
		Permit();
		KPrintF("Can't allocate! Hardware already used by another MHI instance.\n");
		FreeVec(mp->BufferList);
		FreeVec(mp);
		return NULL;
	}
	if(ahi_present_locked(MHI_LibBase)) {
		Permit();
		KPrintF("Can't allocate! Hardware already used by AHI.\n");
		FreeVec(mp->BufferList);
		FreeVec(mp);
		return NULL;
	}
	install_irq_server_locked(mp);
	MHI_LibBase->NumAllocatedDecoders++;
	Permit();

	// Set a balanced Paula-vs-ZZ9000AX output mixer default. AP_DSP_SET_VOLUMES
	// (param 10) encodes AHI/MHI output level in the high byte and Paula pass-
	// through level in the low byte (each 0x00-0xFF). Summing both above
	// ~0x100 starts saturating the DAC (per MNT community forum thread 1011).
	//
	// Default 0xC040 compensates for early ZZ9000AX revisions that carry an
	// opamp on U4 which over-amplifies the Paula pass-through and makes raw
	// Paula dominate over MP3 playback: we boost MHI to 0xC0 (~1.5x) and cut
	// Paula to 0x40 (~0.5x) to pull them toward parity. Users with the
	// fixed-hardware revision (U4 opamp desoldered by MNT) can override via
	// `setenv ZZ9K_MIX_LEVELS 8080` (or any 4-digit hex value) to get a
	// symmetric mix back. (Ref: community.mnt.re/t/zz9000ax-mixing-levels-
	// register/1011)
	setAudioParam(mp, AP_DSP_SET_VOLUMES, read_mix_levels_env(0xC040));

	return mp;
}


/*
 *
 */
// Drain and free every ListNode in the BufferList. Must be called under
// Forbid() because fillFifo() runs from a software interrupt and walks the
// same list -- Forbid prevents the soft IRQ from preempting mid-unlink.
static void drain_buffer_list_locked(struct MhiPlayer *mp) {
	APTR node;
	if(!mp->BufferList) return;
	while((node = RemHead((struct List *)mp->BufferList)) != NULL) {
		FreeVec(node);
	}
}

void i_MHIFreeDecoder(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	if(!mp) return;

	// --- Phase 1: quiesce the hardware and the driver state machine. ---
	// Set Status first so any soft IRQ Cause()d by an in-flight hard ISR
	// (or already queued on Exec's SoftInts list) becomes a no-op when it
	// eventually dispatches -- cdev_sisr early-returns on Status!=PLAYING.
	// Keep the hard ISR INSTALLED here: it is the MHI ownership token on
	// the shared interrupt list, and releasing it before the DSP reset
	// writes finish would let AHI allocate the card and race our own
	// in-flight MMIO (Codex HIGH finding).
	Forbid();
	mp->Status = MHIF_STOPPED;
	disable_hw_audio(mp);
	drain_buffer_list_locked(mp);
	Permit();
	// Permit-to-0 dispatches any soft IRQ still queued on the SoftInts
	// list; it runs cdev_sisr on still-valid mp, sees Status==STOPPED, and
	// returns without touching the list or mp fields. No new soft IRQs can
	// be Cause()d because the hard ISR is disarmed by disable_hw_audio --
	// even though the ISR node is still installed, the HW won't fire it.

	// --- Phase 2: DSP reset while MHI still owns the card. ---
	// The ISR node is still on the interrupt-server list, so ahi_present/
	// FindName(..., "mhizz9000") in a concurrent AHI AllocAudio will still
	// see MHI as the owner and refuse to claim the card during these
	// writes.
	//
	// Quiesce the MP3 decoder before AHI (or a second MHI session) takes
	// the card. i_MHIPlay leaves REG_ZZ_DECODE in DECODE_RUN so the FPGA
	// decoder keeps trying to consume the FIFO and write PCM into our
	// decode_offset region. If we don't flip it back to DECODE_CLEAR
	// here, a subsequent AHI AllocAudio inherits a "running" decoder and
	// can crash on warm boot with ahi.device trap 0x80000006 (mixer
	// reads garbage / overflow trap) -- the user reproduced this on PR
	// #3 after an MP3 -> MOD session.
	setRegister(mp, REG_ZZ_DECODE, DECODE_CLEAR);

	setAudioParam(mp, AP_DSP_SET_STEREO_VOLUME, 100 | (50<<8));
	setAudioParam(mp, AP_DSP_SET_PREFACTOR,     50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND1,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND2,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND3,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND4,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND5,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND6,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND7,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND8,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND9,      50);
	setAudioParam(mp, AP_DSP_SET_EQ_BAND10,     50);

	// --- Phase 3: atomically release ownership. ---
	// Only now do we give up the card: remove the IRQ node AND decrement
	// NumAllocatedDecoders inside a single Forbid window so any racing
	// AllocDecoder / AHI AllocAudio sees both state changes together.
	Forbid();
	if (mp->flags & DEVF_INT2MODE) {
		RemIntServer(INTB_PORTS, &mp->irq);
	} else {
		RemIntServer(INTB_EXTER, &mp->irq);
	}
	if(MHI_LibBase->NumAllocatedDecoders) MHI_LibBase->NumAllocatedDecoders--;
	Permit();

	// --- Phase 4: free memory after ownership is released. ---
	if(mp->BufferList) {
		FreeVec(mp->BufferList);
		mp->BufferList = NULL;
	}
	FreeVec(mp);
}


/*
 *
 */
BOOL i_MHIQueueBuffer(REGA3(APTR mhi_handle), REGA0(APTR mhi_buffer), REGD0(ULONG mhi_size), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;

	if(mp == NULL || mp->BufferList == NULL) return FALSE;

	BufferNode = AllocVec(sizeof(struct ListNode), MEMF_PUBLIC|MEMF_CLEAR);
	if(!BufferNode) {
		// OOM: return FALSE so the caller knows the buffer wasn't queued,
		// instead of dereferencing NULL below.
		KPrintF("MHIQueueBuffer: AllocVec failed\n");
		return FALSE;
	}
	BufferNode->Buffer = mhi_buffer;
	BufferNode->Size   = mhi_size;
	BufferNode->Index  = 0;
	BufferNode->Played = FALSE;

	// Forbid() while linking: fillFifo() walks this list from a software
	// interrupt and must not observe a half-linked tail.
	Forbid();
	AddTail((struct List *)mp->BufferList, (struct Node *)BufferNode);
	Permit();

	KPrintF("MHIQueueBuffer: Adr=0x%08lX\n", mhi_buffer);

	return TRUE;
}


/*
 *
 */
APTR i_MHIGetEmpty(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;
	APTR mhi_buffer = NULL;

	if(!mp || !mp->BufferList) return NULL;

	// Walk + RemHead must be protected against the soft-IRQ fillFifo()
	// that reads from the same list head.
	Forbid();
	for(;;) {
		BufferNode = (struct ListNode *)mp->BufferList->mlh_Head;
		if(BufferNode == NULL) break;
		if(BufferNode->Header.mln_Succ == NULL) break;
		if(BufferNode->Played == FALSE) break;

		mhi_buffer = BufferNode->Buffer;
		RemHead((struct List *)mp->BufferList);
		FreeVec(BufferNode);
	}
	Permit();
	return mhi_buffer;
}


/*
 *
 */
UBYTE i_MHIGetStatus(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(mp) {
		return mp->Status;
	}
	return MHIF_STOPPED;
}


/*
 *
 */
void i_MHIPlay(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	KPrintF("MHIPlay called\n");

	if(!mp) return;

	// Hold Forbid across the entire state transition so Play / Stop / Pause
	// can't interleave. Without this, the "mp->Status = PLAYING; then
	// enable_hw_audio()" ordering (required so cdev_sisr's Status gate is
	// already open for the first soft IRQ) opens a window where a racing
	// Stop could set Status=STOPPED + disable_hw_audio BETWEEN those two
	// statements -- Play would then re-enable the HW with Status=STOPPED,
	// leaving the card interrupting while cdev_sisr silently drops every
	// soft IRQ. All ops inside this Forbid are MMIO writes or static helper
	// logic (no Wait / no blocking I/O), so task preemption blocking is
	// bounded and short.
	Forbid();
	switch(mp->Status) {
		case MHIF_STOPPED:
			KPrintF("MHIPlay: Clearing FIFO.\n");
			clearFifo(mp);
			KPrintF("MHIPlay: Filling FIFO.\n");
			fillFifo(mp);

			// set tx buffer address to 127 MB offset
			setAudioParam(mp, AP_TX_BUF_OFFS_HI, mp->decode_offset>>16);
			setAudioParam(mp, AP_TX_BUF_OFFS_LO, mp->decode_offset&0xffff);

			// set LPF to 20KHz
			setAudioParam(mp, AP_DSP_SET_LOWPASS, 20000);

			// set decoder params
			setDecoderParam(mp, 0, mp->encoded_offset>>16);
			setDecoderParam(mp, 1, mp->encoded_offset&0xffff);
			setDecoderParam(mp, 2, FIFOSIZE>>16);
			setDecoderParam(mp, 3, FIFOSIZE&0xffff);
			setDecoderParam(mp, 4, mp->decode_offset>>16);
			setDecoderParam(mp, 5, mp->decode_offset&0xffff);
			setDecoderParam(mp, 6, mp->decode_chunk_sz>>16);
			setDecoderParam(mp, 7, mp->decode_chunk_sz&0xffff);

			// ZZ_DECODE (init)
			setRegister(mp, REG_ZZ_DECODE, DECODE_INIT);

			// Status must be PLAYING BEFORE enable_hw_audio so cdev_sisr's
			// Status gate is open when the first hard ISR fires.
			mp->Status = MHIF_PLAYING;
			enable_hw_audio(mp);
			KPrintF("MHIPlay: HW audio enabled.\n");
		break;
		case MHIF_PAUSED:
			// Same ordering requirement as the STOPPED case above.
			mp->Status = MHIF_PLAYING;
			enable_hw_audio(mp);
		break;
	}
	Permit();
}


/*
 *
 */
void i_MHIStop(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	KPrintF("MHIStop called\n");

	if(!mp) return;

	// Hold Forbid across the whole dispatch so Stop serializes against a
	// concurrent Play / Pause; the Status check and the disable_hw_audio
	// must be atomic relative to Play's Status=PLAYING + enable_hw_audio.
	Forbid();
	switch(mp->Status) {
		case MHIF_PLAYING:
		case MHIF_PAUSED:
		case MHIF_OUT_OF_DATA:
			// Stop the hardware first so it can't fire a new audio IRQ
			// while we drain. The hard-IRQ server stays installed --
			// it is the ownership claim and is only torn down at
			// FreeDecoder time.
			disable_hw_audio(mp);
			// Flip Status under Forbid so a latent soft IRQ (Cause()d
			// before disable_hw_audio took effect) dispatches into
			// cdev_sisr and early-returns on Status!=PLAYING, leaving
			// buf_offset/FifoWriteIdx/FifoMode alone. Resets live in
			// the same Forbid window so the whole transition is atomic
			// relative to any late soft-IRQ dispatch at Permit-to-0.
			mp->Status = MHIF_STOPPED;
			drain_buffer_list_locked(mp);
			mp->buf_offset = 0;
			mp->FifoWriteIdx = 0;
			mp->FifoMode = FIFO_PREFILL;
		break;
	}
	Permit();
}


/*
 *
 */
void i_MHIPause(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	KPrintF("MHIPause called\n");

	if(!mp) return;

	// Same serialization requirement as Play / Stop: check + disable +
	// status flip must be atomic relative to a racing Play.
	Forbid();
	if(mp->Status == MHIF_PLAYING) {
		disable_hw_audio(mp);
		mp->Status = MHIF_PAUSED;
	}
	Permit();
}


/*
 *
 */
ULONG i_MHIQuery(REGD1( ULONG mhi_query), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	KPrintF("MHIQuery: query = %ld\n", mhi_query);

	switch(mhi_query) {
		case MHIQ_CAPABILITIES:
			return (ULONG)"audio/mpeg{audio/mp3}"; // We currently only support mp3 contained in a raw MPEG stream.

		case MHIQ_DECODER_NAME:
			return (ULONG)"ZZ9000AX";

		case MHIQ_DECODER_VERSION:
			return (ULONG)IDSTRING;

		case MHIQ_AUTHOR:
			return (ULONG)"Thomas Wenzel";

		case MHIQ_IS_HARDWARE:
			return MHIF_TRUE;

//		case MHIQ_LAYER1:
//		case MHIQ_LAYER2:
		case MHIQ_LAYER3:
			return MHIF_SUPPORTED;

		case MHIQ_MPEG1:
		case MHIQ_MPEG2:
		case MHIQ_MPEG25:
		case MHIQ_VARIABLE_BITRATE:
		case MHIQ_JOINT_STEREO:
			return MHIF_SUPPORTED;

		case MHIQ_VOLUME_CONTROL:
			return MHIF_SUPPORTED;
//		case MHIQ_PANNING_CONTROL:
//			return MHIF_SUPPORTED;

		case MHIQ_PREFACTOR_CONTROL:
		case MHIQ_BASS_CONTROL:
		case MHIQ_TREBLE_CONTROL:
		case MHIQ_MID_CONTROL:
		case MHIQ_5_BAND_EQ:
		case MHIQ_10_BAND_EQ:
			return MHIF_SUPPORTED;


		default:
			return MHIF_UNSUPPORTED;
	}
}

/*
 *
 */
void i_MHISetParam(REGA3(APTR mhi_handle), REGD0(UWORD mhi_param), REGD1(ULONG mhi_value), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(mp) {
		switch(mhi_param) {
			case MHIP_PANNING: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				mp->panning = mhi_value;
				// set volume/panning
				setAudioParam(mp, AP_DSP_SET_STEREO_VOLUME, mp->volume | (mp->panning<<8));
				break;

			case MHIP_VOLUME: // 0..100
				if(mhi_value > 100) mhi_value = 100;
				mp->volume = mhi_value;
				// set volume/panning
				setAudioParam(mp, AP_DSP_SET_STEREO_VOLUME, mp->volume | (mp->panning<<8));
				break;

			case MHIP_PREFACTOR: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_PREFACTOR, mhi_value);
				break;

			case MHIP_BAND1:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND1, mhi_value);
				break;
			case MHIP_BAND2:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND2, mhi_value);
				break;
			case MHIP_BAND3:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND3, mhi_value);
				break;
			case MHIP_BAND4:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND4, mhi_value);
				break;
			case MHIP_BAND5:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND5, mhi_value);
				break;
			case MHIP_BAND6:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND6, mhi_value);
				break;
			case MHIP_BAND7:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND7, mhi_value);
				break;
			case MHIP_BAND8:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND8, mhi_value);
				break;
			case MHIP_BAND9:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND9, mhi_value);
				break;
			case MHIP_BAND10:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, AP_DSP_SET_EQ_BAND10, mhi_value);
				break;

			default:
				KPrintF("MHISetParam: Unknown parameter %ld, value = %ld\n", mhi_param, mhi_value);
				break;
		}
	}
}

