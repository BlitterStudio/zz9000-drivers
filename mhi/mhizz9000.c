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
 * Modernized (2026): MP3 decode now runs through the ZZ9000 SDK
 * audio-stream sessions (zz9k.library) -- the card decodes on its second
 * CPU core and the firmware's AX playback pump feeds the audio DMA
 * straight from the session's PCM ring. The legacy register-driven
 * decoder (the ZZ_REG_DECODE register family) is gone on both sides; this
 * driver does no per-period work and PCM never crosses Zorro.
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

#include "zz9000_ax.h"
#include "mhizz9000.h"

#include "zz9k/audio.h"
#include "zz9k/library_vectors.h"
#include "zz9k/shared.h"
#include <proto/zz9k.h>

// KPrintF tracing is compiled out unless the trace build is selected
// (build.sh also produces mhizz9000.library.trace with -DZZ_MHI_TRACE).
// Capture the output on the Amiga with Sashimi.
#ifndef ZZ_MHI_TRACE
#define KPrintF(...)
#endif

// zz9k.library base for the proto inline calls; opened per decoder
// allocation (there is at most one decoder).
struct Library *ZZ9KBase;

// 68k -> card transport chunk (one FEED per chunk). Small on purpose:
// an app buffer completes (and is signalled/reclaimable) only when its
// LAST byte is accepted, so the acceptance quantum bounds how far a
// completion can land from its real-time position. 4K at 128 kbit/s is
// ~250 ms; a 32K quantum bunched completions into multi-second jumps
// of the player's time display.
#define ZZ_MHI_STAGING_BYTES   (4UL * 1024UL)
// Card-side compressed ring. The size is a LIVENESS constraint, not
// just tuning: a strictly signal-driven app (AmigaAMP) only services
// the driver when a buffer completes, so (a) the ring plus the PCM
// ring must absorb the app's ENTIRE first buffer during Play's
// prebuffer or the first completion signal never fires and playback
// never starts, and (b) the card-side runway must outlast one full
// app-buffer period between completion signals or steady-state
// underruns. AmigaAMP queues 32K buffers; 32K here (+ ~350 ms decoded
// PCM) satisfies both at any bitrate, matches the legacy driver's
// 32K FIFO fill target, and keeps the seek/display lead at the legacy
// ~2 s. Do not shrink it below the largest app buffer size.
#define ZZ_MHI_MP3_RING_BYTES  (32UL * 1024UL)
// Card-side PCM ring the firmware pump plays from: 16 periods of 3840
// bytes (~320 ms of 48 kHz stereo).
#define ZZ_MHI_PCM_RING_BYTES  (61440UL)
// Pump refill threshold: half the PCM ring.
#define ZZ_MHI_PCM_LOW_WATER   (ZZ_MHI_PCM_RING_BYTES / 2UL)

/* ******************************** */
/*  BEGIN ZZ9000AX parameter access */
/*  Don't worry!                    */
/*  The compiler inlines these!     */
/* ******************************** */
static void setRegister(struct MhiPlayer *mp, ULONG Register, UWORD Value) {
	*((volatile UWORD*)(mp->hw_addr+Register)) = Value;
}

static void setAudioParam(struct MhiPlayer *mp, ULONG Param, UWORD Value) {
	*((volatile UWORD*)(mp->hw_addr+ZZ_REG_AUDIO_PARAM)) = Param;
	*((volatile UWORD*)(mp->hw_addr+ZZ_REG_AUDIO_VAL))	 = Value;
	*((volatile UWORD*)(mp->hw_addr+ZZ_REG_AUDIO_PARAM)) = 0;
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
	fh = Open((CONST_STRPTR)ZZ_AX_MIX_LEVELS_ENV, MODE_OLDFILE);
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

// Check whether AHI has its ISR installed on our shared interrupt level.
// MUST be called with Forbid() already active so the caller can combine
// this check with AddIntServer() into a single atomic claim step --
// otherwise AHI and MHI can both win the check and then both attach.
// The IntVects[] server list can be mutated by any task, so walking it
// unprotected would be unsafe in its own right.
static BOOL ahi_present_locked(struct MHI_LibBase *MhiLibBase) {
	struct List *IrqList;
	if(MhiLibBase->flags & ZZ_AX_DEVF_INT2MODE) {
		IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
	}
	else {
		IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
	}
	return FindName(IrqList, ZZ_AX_IRQ_NAME_AHI) ? TRUE : FALSE;
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
	if((cd = (struct ConfigDev*)FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER,
	                                          ZZ9000_PRODUCT_Z3))) {
		MhiLibBase->zorro_version = 3;
		KPrintF("ZZ9000 Zorro 3 Version detected.\n");
	}
	else if((cd = (struct ConfigDev*)FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER,
	                                               ZZ9000_PRODUCT_Z2))) {
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

	// ZZ_REG_AUDIO_CONFIG bit 0 is the "AX present" strap; mask explicitly
	// so other status bits can't ever make detection look successful.
	ax_present = (*((volatile UWORD*)(hw_addr+ZZ_REG_AUDIO_CONFIG))) & 1;
	if(!ax_present) {
		KPrintF("Error: ZZ9000AX not detected.\n");
		return FALSE;
	}

	KPrintF("HwAddr=0x%08lX, HwSize=0x%08lX\n", hw_addr, hw_size);
	MhiLibBase->hw_addr = hw_addr;
	MhiLibBase->hw_size = hw_size;

	BPTR fh;
	if((fh=Open((CONST_STRPTR)ZZ_AX_INT2_ENV, MODE_OLDFILE))) {
		Close(fh);
		MhiLibBase->flags |= ZZ_AX_DEVF_INT2MODE;
	}

	return TRUE;
}

void UserLibCleanup(struct MHI_LibBase *MhiLibBase) {
	// Nothing to clean up here because UserLibInit() didn't leave anything open.
}

/* ********************* */
/*  BEGIN feed engine    */
/* ********************* */

static void mhi_signal_app(struct MhiPlayer *mp) {
	if(mp->MhiTask && mp->MhiMask) Signal(mp->MhiTask, mp->MhiMask);
}

// Feed queued application data to the card's compressed ring. TASK
// CONTEXT ONLY: ZZ9KAudioStreamFeed blocks on the mailbox completion, so
// this must never run under Forbid() or from an interrupt. The buffer
// list is only snapshotted/updated inside short Forbid windows;
// mp->list_gen invalidates the in-flight chunk if a concurrent
// Stop/Free drained the list while the mailbox call was blocking.
static void mhi_feed_pending(struct MhiPlayer *mp) {
	if(mp->session == 0) return;

	for(;;) {
		struct ListNode *node = NULL;
		struct ListNode *it;
		UBYTE *src = NULL;
		ULONG index = 0;
		ULONG chunk = 0;
		ULONG gen;
		ZZ9KAudioStreamFeedDesc feed;

		Forbid();
		gen = mp->list_gen;
		for(it = (struct ListNode *)mp->BufferList->mlh_Head;
		    it->Header.mln_Succ;
		    it = (struct ListNode *)it->Header.mln_Succ) {
			if(it->Played == FALSE) {
				node = it;
				index = node->Index;
				src = node->Buffer + index;
				chunk = node->Size - index;
				break;
			}
		}
		Permit();

		if(!node) {
			mp->have_unfed = FALSE;
			return;
		}
		mp->have_unfed = TRUE;

		if(chunk > ZZ_MHI_STAGING_BYTES) chunk = ZZ_MHI_STAGING_BYTES;

		KPrintF("feed: node=0x%08lX idx=%lu chunk=%lu\n",
		        (ULONG)node, (ULONG)index, (ULONG)chunk);

		// Skip the 68k->card copy when this exact chunk already sits in
		// the staging buffer from a backpressured attempt: the retry
		// then costs one mailbox op, not a 32K Zorro copy per pacing
		// wake-up.
		if(!(mp->staged_valid && mp->staged_gen == gen &&
		     mp->staged_node == (APTR)node && mp->staged_index == index &&
		     mp->staged_chunk == chunk)) {
			if(!zz9k_shared_copy_to(&mp->staging, 0, src, chunk)) return;
			mp->staged_node  = (APTR)node;
			mp->staged_index = index;
			mp->staged_chunk = chunk;
			mp->staged_gen   = gen;
			mp->staged_valid = TRUE;
		}
		if(!zz9k_audio_build_stream_feed_desc(&feed, mp->session,
		                                      mp->staging.handle, 0,
		                                      chunk, 0)) return;
		if(ZZ9KAudioStreamFeed(&feed, &mp->result) != ZZ9K_STATUS_OK)
			return;

		if(mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) {
			// Card ring full; nothing was consumed. A later entry
			// point retries from task context (the staged memo above
			// makes that retry cheap).
			if(!mp->backpressure)
				KPrintF("feed: backpressure (flags=0x%08lX)\n",
				        (ULONG)mp->result.flags);
			mp->backpressure = TRUE;
			return;
		}
		if(mp->backpressure)
			KPrintF("feed: backpressure cleared\n");
		mp->backpressure = FALSE;
		mp->staged_valid = FALSE;   // accepted: key must never be reused

		Forbid();
		if(gen == mp->list_gen) {
			node->Index += chunk;
			if(node->Index >= node->Size) {
				// Fully handed to the card: the app may reclaim it
				// via MHIGetEmpty (same semantics as the legacy
				// FIFO copy completion).
				node->Played = TRUE;
				mhi_signal_app(mp);
			}
		}
		Permit();
		if(gen != mp->list_gen) return;   // drained under us: stop
	}
}

// Open the SDK session (and, once, the shared buffers backing it).
static BOOL mhi_stream_open(struct MhiPlayer *mp) {
	ZZ9KAudioStreamBeginDesc begin;

	if(mp->session != 0) return TRUE;

	if(!mp->rings_allocated) {
		if(ZZ9KAllocShared(ZZ_MHI_STAGING_BYTES, 16, 0,
		                   &mp->staging) != ZZ9K_STATUS_OK) {
			KPrintF("stream_open: staging alloc failed\n");
			return FALSE;
		}
		if(ZZ9KAllocShared(ZZ_MHI_MP3_RING_BYTES, 16, 0,
		                   &mp->mp3_ring) != ZZ9K_STATUS_OK) {
			KPrintF("stream_open: mp3 ring alloc failed\n");
			ZZ9KFreeShared(mp->staging.handle);
			return FALSE;
		}
		if(ZZ9KAllocShared(ZZ_MHI_PCM_RING_BYTES, 16, 0,
		                   &mp->pcm_ring) != ZZ9K_STATUS_OK) {
			KPrintF("stream_open: pcm ring alloc failed\n");
			ZZ9KFreeShared(mp->mp3_ring.handle);
			ZZ9KFreeShared(mp->staging.handle);
			return FALSE;
		}
		mp->rings_allocated = TRUE;
	}

	// S16LE: the AX audio DMA consumes little-endian samples (the legacy
	// on-card decoder produced exactly that). low_water is the firmware
	// pump's PCM refill threshold.
	if(!zz9k_audio_build_stream_begin_desc(
	        &begin, mp->mp3_ring.handle, mp->mp3_ring.length,
	        mp->pcm_ring.handle, mp->pcm_ring.length, 0, 0,
	        ZZ9K_AUDIO_SAMPLE_FORMAT_S16LE,
	        ZZ_MHI_PCM_LOW_WATER, 0, 0)) {
		KPrintF("stream_open: begin desc rejected (client side)\n");
		return FALSE;
	}
	if(ZZ9KAudioStreamBegin(&begin, &mp->result) != ZZ9K_STATUS_OK) {
		KPrintF("stream_open: BEGIN rejected\n");
		return FALSE;
	}

	mp->session = mp->result.session;
	mp->backpressure = FALSE;
	KPrintF("stream_open: session=%lu\n", (ULONG)mp->session);
	return TRUE;
}

// Stop AX playback and close the session (Stop/Free semantics; Pause
// keeps the session so PLAY resumes gaplessly). Task context only.
static void mhi_stream_close(struct MhiPlayer *mp) {
	ZZ9KAudioStreamResult r;

	if(mp->session == 0) return;
	(void)ZZ9KAudioStreamStop(mp->session, 0, &r);
	(void)ZZ9KAudioStreamClose(mp->session, 0, &r);
	mp->session = 0;
	mp->staged_valid = FALSE;
	mp->play_pending = FALSE;
}

// Complete a deferred Play: bind the session to the AX output once the
// card has decoded PCM and knows the sample rate. MHIPlay may legally
// arrive BEFORE any data is queued (the legacy driver allowed it, and
// seeking apps rely on it: Stop, Play, then requeue from the new file
// position), so Play sets play_pending and the bind happens here, from
// whichever entry point feeds the decisive chunk. Task context only.
static void mhi_try_bind(struct MhiPlayer *mp) {
	if(!mp->play_pending || mp->session == 0) return;
	if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) == 0 ||
	   mp->result.sample_rate == 0) {
		return;
	}
	if(ZZ9KAudioStreamPlay(mp->session, 0, &mp->result) != ZZ9K_STATUS_OK) {
		KPrintF("mhi_try_bind: PLAY rejected.\n");
		return;
	}
	// A Stop/Pause that raced the mailbox call above already unbound
	// the session card-side; nothing to undo here.
	mp->play_pending = FALSE;
	KPrintF("mhi_try_bind: session bound to AX output.\n");
}

// Standard entry-point service: move queued data to the card, then
// finish a deferred Play if the decisive data just arrived.
static void mhi_service(struct MhiPlayer *mp) {
	mhi_feed_pending(mp);
	mhi_try_bind(mp);
}

/* ******************* */
/*  END feed engine    */
/* ******************* */

extern ULONG dev_sisr(struct MhiPlayer *mp asm("a1"));
ULONG cdev_sisr(struct MhiPlayer *mp asm("a1")) {
	// Deliberately inert. The signal contract is STRICT (same as the
	// legacy driver): the application is signalled exactly when a
	// buffer COMPLETES, from the feed engine in task context, at real
	// playback pace. MHI applications account playback time by those
	// signals, so ANY extra signal -- a pacing storm or even a
	// throttled keep-alive -- inflates their elapsed/total time
	// display (bench: 50 Hz signals made the counter race; a ~1.3 s
	// keep-alive still overshot a 4:06 track to 4:42). Feeding is
	// driven entirely by the app's own entry points
	// (GetStatus/QueueBuffer/GetEmpty all run the feed engine); the
	// card holds enough audio to bridge normal polling gaps. The
	// interrupt machinery itself stays installed: the ISR node on the
	// shared server list is the MHI-vs-AHI ownership token.
	return 0;
}

extern ULONG dev_isr(struct MhiPlayer *mp asm("a1"));
ULONG cdev_isr(struct MhiPlayer *mp asm("a1")) {
	UWORD status = *(UWORD*)(mp->hw_addr+ZZ_REG_CONFIG);

	// audio interrupt signal set? (ZZ_REG_CONFIG bit 1, mask 0x02)
	if(status & 2) {
		// Ack/clear audio interrupt.
		*(USHORT*)(mp->hw_addr+ZZ_REG_CONFIG) = 8|32;
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
	mp->sirq.is_Node.ln_Name = ZZ_AX_IRQ_NAME_MHI_SOFT;
	mp->sirq.is_Data = mp;
	mp->sirq.is_Code = (void*)dev_sisr;

	mp->irq.is_Node.ln_Type = NT_INTERRUPT;
	mp->irq.is_Node.ln_Pri  = 126; // High priority: this ISR must react quickly.
	mp->irq.is_Node.ln_Name = ZZ_AX_IRQ_NAME_MHI;
	mp->irq.is_Data = mp;
	mp->irq.is_Code = (void*)dev_isr;
}

// Install the hard interrupt server. MUST be called under Forbid() so the
// caller can combine it with ahi_present_locked() atomically.
static void install_irq_server_locked(struct MhiPlayer *mp) {
	if (mp->flags & ZZ_AX_DEVF_INT2MODE) {
		AddIntServer(INTB_PORTS, &mp->irq);
	} else {
		AddIntServer(INTB_EXTER, &mp->irq);
	}
}

// Keep the HW-side audio interrupt OFF. This driver never enables it:
// the signal contract is buffer-completion-only (see cdev_sisr), and
// on the firmware side flipping this bit also silences/clears the TX
// ring -- a needless dropout. The disable is kept as hygiene on
// Stop/Pause/Free so an earlier AHI session can't leave the interrupt
// armed while MHI owns the card.
static void disable_hw_audio(struct MhiPlayer *mp) {
	setRegister(mp, ZZ_REG_AUDIO_CONFIG, 0);
}

/*
 *
 */
APTR i_MHIAllocDecoder(REGA0(struct Task *mhi_task), REGD0(ULONG mhi_sigmask), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = NULL;

	// The modern decoder path runs through zz9k.library audio-stream
	// sessions plus the firmware's AX playback binding; require both.
	if(!ZZ9KBase) {
		ZZ9KBase = OpenLibrary((STRPTR)"zz9k.library", 0);
	}
	if(!ZZ9KBase) {
		KPrintF("Can't open zz9k.library.\n");
		return NULL;
	}
	if(ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_AUDIO_PLAYBACK) {
		KPrintF("zz9k.library too old for audio playback.\n");
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
		return NULL;
	}

	mp = AllocVec(sizeof(struct MhiPlayer), MEMF_CLEAR);
	if(!mp) {
		KPrintF("Can't allocate MhiPlayer.\n");
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
		return NULL;
	}

	mp->hw_addr = MHI_LibBase->hw_addr;
	mp->flags   = MHI_LibBase->flags;

	mp->MhiTask = mhi_task;
	mp->MhiMask = mhi_sigmask;
	mp->Status  = MHIF_STOPPED;

	mp->volume  = 100;
	mp->panning = 50;

	mp->BufferList = AllocVec(sizeof(struct MinList), MEMF_PUBLIC|MEMF_CLEAR);
	if(!mp->BufferList) {
		FreeVec(mp);
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
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
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
		return NULL;
	}
	if(ahi_present_locked(MHI_LibBase)) {
		Permit();
		KPrintF("Can't allocate! Hardware already used by AHI.\n");
		FreeVec(mp->BufferList);
		FreeVec(mp);
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
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
	// Default 0x8080 is the symmetric baseline that matches the fixed-
	// hardware revision every new customer receives (MNT removed the U4
	// opamp that over-amplified Paula pass-through on early R1 units).
	// Owners of an unfixed early R1 where raw Paula dominates over MP3
	// output can compensate via `setenv ZZ9K_MIX_LEVELS C040` (boost MHI,
	// cut Paula) — or any other 4-digit hex value — without rebuilding.
	// (Ref: community.mnt.re/t/zz9000ax-mixing-levels-register/1011)
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_VOLUMES,
	              read_mix_levels_env(ZZ_AX_DEFAULT_MIX_LEVELS));

	return mp;
}


/*
 *
 */
// Drain and free every ListNode in the BufferList. Must be called under
// Forbid() so a feed snapshot in another task never observes a
// half-unlinked list; list_gen invalidates any feed already blocking on
// the mailbox.
static void drain_buffer_list_locked(struct MhiPlayer *mp) {
	APTR node;
	if(!mp->BufferList) return;
	while((node = RemHead((struct List *)mp->BufferList)) != NULL) {
		FreeVec(node);
	}
	mp->list_gen++;
	mp->have_unfed = FALSE;
}

void i_MHIFreeDecoder(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	if(!mp) return;

	// --- Phase 1: quiesce the hardware and the driver state machine. ---
	// Set Status first so any soft IRQ Cause()d by an in-flight hard ISR
	// (or already queued on Exec's SoftInts list) becomes a no-op when it
	// eventually dispatches -- cdev_sisr early-returns on Status!=MHIF_PLAYING.
	// Keep the hard ISR INSTALLED here: it is the MHI ownership token on
	// the shared interrupt list, and releasing it before the teardown
	// writes finish would let AHI allocate the card and race our own
	// in-flight MMIO (Codex HIGH finding).
	Forbid();
	mp->Status = MHIF_STOPPED;
	mp->play_pending = FALSE;
	disable_hw_audio(mp);
	drain_buffer_list_locked(mp);
	Permit();
	// Permit-to-0 dispatches any soft IRQ still queued on the SoftInts
	// list; it runs cdev_sisr on still-valid mp, sees Status==STOPPED, and
	// returns without touching the list or mp fields. No new soft IRQs can
	// be Cause()d because the hard ISR is disarmed by disable_hw_audio --
	// even though the ISR node is still installed, the HW won't fire it.

	// --- Phase 2: decoder teardown while MHI still owns the card. ---
	// The ISR node is still on the interrupt-server list, so ahi_present/
	// FindName(..., "mhizz9000") in a concurrent AHI AllocAudio will still
	// see MHI as the owner and refuse to claim the card during these
	// steps.
	//
	// Quiesce the decoder before AHI (or a second MHI session) takes the
	// card: stop the firmware's AX playback binding and close the SDK
	// session (this replaces the legacy DECODE_CLEAR -- same reason: an
	// inherited "running" decoder crashed ahi.device with trap 0x80000006
	// on warm boot after an MP3 -> MOD session, reproduced on PR #3).
	mhi_stream_close(mp);
	if(mp->rings_allocated) {
		ZZ9KFreeShared(mp->pcm_ring.handle);
		ZZ9KFreeShared(mp->mp3_ring.handle);
		ZZ9KFreeShared(mp->staging.handle);
		mp->rings_allocated = FALSE;
	}

	setAudioParam(mp, ZZ_AX_AP_DSP_SET_STEREO_VOLUME, 100 | (50<<8));
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_PREFACTOR,     50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND1,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND2,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND3,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND4,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND5,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND6,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND7,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND8,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND9,      50);
	setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND10,     50);

	// --- Phase 3: atomically release ownership. ---
	// Only now do we give up the card: remove the IRQ node AND decrement
	// NumAllocatedDecoders inside a single Forbid window so any racing
	// AllocDecoder / AHI AllocAudio sees both state changes together.
	Forbid();
	if (mp->flags & ZZ_AX_DEVF_INT2MODE) {
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

	if(ZZ9KBase) {
		CloseLibrary(ZZ9KBase);
		ZZ9KBase = NULL;
	}
}


/*
 *
 */
BOOL i_MHIQueueBuffer(REGA3(APTR mhi_handle), REGA0(APTR mhi_buffer), REGD0(ULONG mhi_size), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;

	if(mp == NULL || mp->BufferList == NULL || !mhi_buffer || mhi_size == 0) {
		return FALSE;
	}

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

	// Forbid() while linking: a feed snapshot in another task must not
	// observe a half-linked tail.
	Forbid();
	AddTail((struct List *)mp->BufferList, (struct Node *)BufferNode);
	mp->have_unfed = TRUE;
	Permit();

	KPrintF("MHIQueueBuffer: Adr=0x%08lX Size=%lu\n", (ULONG)mhi_buffer,
	        (ULONG)mhi_size);

	// Opportunistic feed from task context: while playing (or prebuffered
	// before Play), push as much of the queue to the card as it accepts,
	// and complete a deferred Play once the card is ready.
	if(mp->session != 0) {
		mhi_service(mp);
	}

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

	// Walk + RemHead must be protected against a feed snapshot in another
	// task that reads from the same list head. Return one completed buffer
	// per call: callers use repeated GetEmpty calls to reclaim every
	// buffer, and draining multiple nodes here would lose all but the last
	// pointer.
	Forbid();
	BufferNode = (struct ListNode *)mp->BufferList->mlh_Head;
	if(BufferNode != NULL &&
	   BufferNode->Header.mln_Succ != NULL &&
	   BufferNode->Played != FALSE) {
		mhi_buffer = BufferNode->Buffer;
		RemHead((struct List *)mp->BufferList);
		FreeVec(BufferNode);
	}
	Permit();

	if(mhi_buffer)
		KPrintF("MHIGetEmpty: Adr=0x%08lX\n", (ULONG)mhi_buffer);

	// Keep the card fed whenever the app services the driver.
	if(mp->session != 0 && mp->Status == MHIF_PLAYING) {
		mhi_service(mp);
	}
	return mhi_buffer;
}


/*
 *
 */
UBYTE i_MHIGetStatus(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(!mp) return MHIF_STOPPED;

	if(mp->Status == MHIF_PLAYING && mp->session != 0) {
#ifdef ZZ_MHI_TRACE
		static ULONG status_calls;
		if((++status_calls & 63) == 0)
			KPrintF("MHIGetStatus: #%lu unfed=%ld bp=%ld pend=%ld\n",
			        status_calls, (LONG)mp->have_unfed,
			        (LONG)mp->backpressure, (LONG)mp->play_pending);
#endif
		// Service the feed while we're here (task context).
		mhi_service(mp);
		if(!mp->have_unfed && !mp->play_pending) {
			// Everything is on the card; probe it. PLAY on the
			// already-bound session is idempotent and returns fresh
			// stream state: no PCM left and nothing queued means the
			// stream has drained. (Skipped while a deferred Play is
			// still waiting for data -- an empty new session must not
			// read as end-of-stream.)
			ZZ9KAudioStreamResult r;
			if(ZZ9KAudioStreamPlay(mp->session, 0, &r) == ZZ9K_STATUS_OK &&
			   (r.flags & ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) == 0) {
				KPrintF("MHIGetStatus: OUT_OF_DATA\n");
				return MHIF_OUT_OF_DATA;
			}
		}
	}
	return mp->Status;
}


/*
 *
 */
void i_MHIPlay(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	int tries;

	KPrintF("MHIPlay called\n");

	if(!mp) return;

	switch(mp->Status) {
		case MHIF_STOPPED:
		case MHIF_OUT_OF_DATA:
			if(!mhi_stream_open(mp)) {
				KPrintF("MHIPlay: session open failed.\n");
				return;
			}

			// set LPF to 20KHz
			setAudioParam(mp, ZZ_AX_AP_DSP_SET_LOWPASS, 20000);

			// Prebuffer: push queued data until the card has decoded
			// PCM and knows the stream's sample rate (the firmware
			// requires that before binding the session to the AX
			// output). Bounded: each pass feeds up to the whole queue.
			for(tries = 8; tries > 0; tries--) {
				mhi_feed_pending(mp);
				if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) &&
				   mp->result.sample_rate != 0) break;
				if(!mp->have_unfed || mp->backpressure) break;
			}
			KPrintF("MHIPlay: prebuffer flags=0x%08lX rate=%lu unfed=%ld\n",
			        (ULONG)mp->result.flags,
			        (ULONG)mp->result.sample_rate,
			        (LONG)mp->have_unfed);

			// Report PLAYING right away and bind when the card is
			// ready. Play may legally arrive with an empty (or too
			// short) queue -- notably on a seek, where the app stops,
			// calls Play, and requeues from the new file position --
			// so a not-yet-ready card must not fail the call.
			Forbid();
			mp->Status = MHIF_PLAYING;
			mp->play_pending = TRUE;
			Permit();
			mhi_try_bind(mp);
		break;
		case MHIF_PAUSED:
			if(mp->play_pending) {
				// Paused before the deferred bind completed: just
				// rearm; the bind happens when data arrives.
				Forbid();
				mp->Status = MHIF_PLAYING;
				Permit();
				mhi_try_bind(mp);
				break;
			}
			// Resume: the session (and its PCM ring) survived Pause,
			// so re-binding continues gaplessly.
			if(ZZ9KAudioStreamPlay(mp->session, 0, &mp->result) != ZZ9K_STATUS_OK) {
				KPrintF("MHIPlay: resume rejected.\n");
				return;
			}
			mp->Status = MHIF_PLAYING;
		break;
	}
}


/*
 *
 */
void i_MHIStop(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	ULONG old;

	KPrintF("MHIStop called\n");

	if(!mp) return;

	// Serialize the state flip against Play / Pause and any latent soft
	// IRQ: disable the HW interrupt and flip Status inside one Forbid
	// window (cdev_sisr early-returns on Status!=PLAYING), then do the
	// blocking mailbox teardown OUTSIDE Forbid.
	Forbid();
	old = mp->Status;
	disable_hw_audio(mp);
	mp->Status = MHIF_STOPPED;
	mp->play_pending = FALSE;
	drain_buffer_list_locked(mp);
	mp->backpressure = FALSE;
	Permit();

	if(old != MHIF_STOPPED) {
		// Stop resets the position: close the session entirely; the
		// next Play begins a fresh one (rings are reused).
		mhi_stream_close(mp);
	}
}


/*
 *
 */
void i_MHIPause(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	BOOL paused = FALSE;
	ZZ9KAudioStreamResult r;

	KPrintF("MHIPause called\n");

	if(!mp) return;

	// Same serialization requirement as Play / Stop: check + disable +
	// status flip must be atomic relative to a racing Play. The mailbox
	// call happens after, outside Forbid.
	Forbid();
	if(mp->Status == MHIF_PLAYING) {
		disable_hw_audio(mp);
		mp->Status = MHIF_PAUSED;
		paused = TRUE;
	}
	Permit();

	if(paused && mp->session != 0) {
		// Unbind from the AX output; the session and its decoded PCM
		// survive, so Play resumes exactly where we stopped.
		(void)ZZ9KAudioStreamStop(mp->session, 0, &r);
	}
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
			return (ULONG)"ZZ9000AX (SDK core-1)";

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
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_STEREO_VOLUME, mp->volume | (mp->panning<<8));
				break;

			case MHIP_VOLUME: // 0..100
				if(mhi_value > 100) mhi_value = 100;
				mp->volume = mhi_value;
				// set volume/panning
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_STEREO_VOLUME, mp->volume | (mp->panning<<8));
				break;

			case MHIP_PREFACTOR: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_PREFACTOR, mhi_value);
				break;

			case MHIP_BAND1:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND1, mhi_value);
				break;
			case MHIP_BAND2:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND2, mhi_value);
				break;
			case MHIP_BAND3:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND3, mhi_value);
				break;
			case MHIP_BAND4:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND4, mhi_value);
				break;
			case MHIP_BAND5:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND5, mhi_value);
				break;
			case MHIP_BAND6:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND6, mhi_value);
				break;
			case MHIP_BAND7:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND7, mhi_value);
				break;
			case MHIP_BAND8:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND8, mhi_value);
				break;
			case MHIP_BAND9:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND9, mhi_value);
				break;
			case MHIP_BAND10:
				if(mhi_value > 100) mhi_value = 100;
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_EQ_BAND10, mhi_value);
				break;

			default:
				KPrintF("MHISetParam: Unknown parameter %ld, value = %ld\n", mhi_param, mhi_value);
				break;
		}
	}
}
