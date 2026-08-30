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
#include <devices/timer.h>

#include "zz9000_ax.h"
#include "zzcfg_query.h"
#include "mhizz9000.h"
#include "transport_geometry.h"

#include "zz9k/audio.h"
#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
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

/* Transport geometry is selected per bus generation in
 * transport_geometry.h. Z2 keeps the qualified compact profile. Z3 uses the
 * same 64-KiB feed / 128-KiB compressed / 256-KiB PCM geometry as ZZPlay's
 * passing accelerated-AHI path, avoiding the measured 4-KiB MHI mailbox storm. */
// Public MHI has no EOF call. Sustained queue starvation therefore requests
// a RESUMABLE drain: firmware decodes every complete frame, retains a partial
// compressed frame, drains the real AX tail, and reports OUT_OF_DATA while
// remaining ready for later buffers. With the feeder's adaptive backoff four
// probes allow roughly 350 ms for a normal client to refill first.
#define ZZ_MHI_DRAIN_IDLE_POLLS  4U

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

// Firmware-authoritative control plane (R4/R16): submit this owner's
// neutral source trim through the ZZ9K_OP_AUDIO_TRIM_SUBMIT mailbox
// opcode. The firmware combines it with the operator's baseline under
// scene policy and owns every master-chain write; the reply's
// applied/bound words are the authority, so the driver never mirrors
// them into DSP registers. Requires ZZ9KBase to be live (claim winner)
// and must run outside Forbid() -- ZZ9KCall blocks on the mailbox
// completion. Failure is non-fatal: playback continues exactly as
// before, only the trim is lost (e.g. transient firmware error).
static void submit_source_trim(void) {
	ZZ9KRequest request;
	ZZ9KMailboxEntry reply;
	ZZ9KAudioTrimSubmitPayload *payload;
	int status;

	zz9k_request_init(&request, ZZ9K_OP_AUDIO_TRIM_SUBMIT);
	request.entry.payload_len = sizeof(ZZ9KAudioTrimSubmitPayload);
	payload = (ZZ9KAudioTrimSubmitPayload *)request.entry.payload.inline_data;
	zz9k_put_be32(payload->balance, ZZ9K_AUDIO_BALANCE_NEUTRAL);
	zz9k_put_be32(payload->flags, 0);

	status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
	if(status == ZZ9K_STATUS_OK) {
		// The reply payload is byte storage; copy it out before reading
		// typed fields (mirrors the SDK reply-extraction convention).
		ZZ9KAudioTrimResultPayload result;
		memcpy(&result, reply.payload.inline_data, sizeof(result));
		if(zz9k_get_be32(result.flags) & ZZ9K_AUDIO_TRIM_RESULT_BOUNDED) {
			KPrintF("MHI: neutral trim bounded by scene policy.\n");
		}
	}
}

/* ****************************** */
/*  END ZZ9000AX parameter access */
/* ****************************** */

// Identify the mode of an active AHI owner from its installed IRQ nodes.
// Qualified AHI publishes a fabric sentinel in addition to the legacy node
// retained for old MHI binaries; check fabric first. Fallback and old AHI
// publish only the legacy node. MUST run under Forbid().
#define MHI_AHI_MODE_NONE   0U
#define MHI_AHI_MODE_LEGACY 1U
#define MHI_AHI_MODE_FABRIC 2U
static UBYTE ahi_mode_locked(ULONG flags) {
	struct List *IrqList;

	if(flags & ZZ_AX_DEVF_INT2MODE) {
		IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
	}
	else {
		IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
	}
	if(FindName(IrqList, ZZ_AX_IRQ_NAME_AHI_FABRIC))
		return MHI_AHI_MODE_FABRIC;
	if(FindName(IrqList, ZZ_AX_IRQ_NAME_AHI))
		return MHI_AHI_MODE_LEGACY;
	return MHI_AHI_MODE_NONE;
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
	UWORD cfg_present = 0;
	if((fh=Open((CONST_STRPTR)ZZ_AX_INT2_ENV, MODE_OLDFILE))) {
		Close(fh);
		MhiLibBase->flags |= ZZ_AX_DEVF_INT2MODE;
	} else if (zzcfg_query(hw_addr, ZZ_CFG_KEY_INT2, &cfg_present) &&
			cfg_present) {
		// `int2 = on` in ZZ9000.CFG (firmware 2.3+)
		MhiLibBase->flags |= ZZ_AX_DEVF_INT2MODE;
	}

	// The mix-levels env override is gone (R11): Paula/AX balance intent
	// now flows through the firmware control plane's operator baseline.
	// Probe for the stale variable's existence only -- never parse a
	// value -- and tell operators still carrying the old early-R1 remedy
	// where the replacement lives. kprintf (not the trace-gated KPrintF)
	// so the one-liner is always visible on the debug channel.
	if((fh=Open((CONST_STRPTR)"ENV:ZZ9K_MIX_LEVELS", MODE_OLDFILE))) {
		Close(fh);
		kprintf((CONST_STRPTR)"MHI: ENV:ZZ9K_MIX_LEVELS is ignored; "
		       "balance needs matched firmware (scene baseline).\n");
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

static void mhi_wake_feeder(struct MhiPlayer *mp);

// Mark fully-submitted application buffers reusable only when the firmware's
// cumulative decoder cursor has reached their end. A completed resumable
// drain may release the accepted boundary as a whole: any incomplete frame
// is retained card-side, so the application's source memory is no longer
// needed even though those final bytes are intentionally not discarded.
static void mhi_complete_consumed(struct MhiPlayer *mp, BOOL drain_all) {
	struct ListNode *node;
	BOOL notify = FALSE;

	if(!mp || !mp->BufferList) return;
	Forbid();
	for(node = (struct ListNode *)mp->BufferList->mlh_Head;
	    node->Header.mln_Succ;
	    node = (struct ListNode *)node->Header.mln_Succ) {
		if(node->Played != FALSE)
			continue;
		if(node->Index < node->Size || !node->StreamEndValid ||
		   (!drain_all &&
		    (LONG)(mp->result.bytes_consumed - node->StreamEnd) < 0))
			break;
		node->Played = TRUE;
		notify = TRUE;
	}
	Permit();
	if(notify) mhi_signal_app(mp);
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
			if(it->Index < it->Size) {
				node = it;
				index = node->Index;
				src = node->Buffer + index;
				chunk = node->Size - index;
				break;
			}
		}
		// Publish the queued-data flag atomically with the scan, still
		// under Forbid: MHIQueueBuffer also sets have_unfed = TRUE under
		// Forbid, so clearing it outside this window would race a
		// concurrent queue -- the feeder could observe an empty list, drop
		// Forbid, and then overwrite a just-queued TRUE back to FALSE.
		// MHIGetStatus would then probe the (still unfed) card, latch
		// MHIF_OUT_OF_DATA, and -- because the feeder only feeds while
		// PLAYING -- the buffer would stall until the next queue flipped
		// the state again. Whoever runs last under Forbid wins, and
		// QueueBuffer always sets TRUE + wakes us after AddTail, so a node
		// queued after our Permit is never lost.
		mp->have_unfed = (node != NULL);
		Permit();

		if(!node)
			return;

		if(chunk > mp->staging.length) chunk = mp->staging.length;

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
			mhi_complete_consumed(mp, FALSE);
			// Card ring full; nothing was consumed. A later entry
			// point retries from task context (the staged memo above
			// makes that retry cheap).
			if(!mp->backpressure) {
				KPrintF("feed: backpressure (flags=0x%08lX)\n",
				        (ULONG)mp->result.flags);
			}
			mp->backpressure = TRUE;
			return;
		}
		if(mp->backpressure) {
			KPrintF("feed: backpressure cleared\n");
		}
		mp->backpressure = FALSE;
		mp->staged_valid = FALSE;   // accepted: key must never be reused
		mp->feeds_accepted++;       // resets the feeder's retry backoff

		Forbid();
		if(gen == mp->list_gen) {
			/* Any accepted non-empty FEED cancels firmware drain state.
			 * Mirror that transition locally even when QueueBuffer raced an
			 * in-flight DRAIN response. */
			mp->drain_requested = FALSE;
			mp->starvation_polls = 0;
			node->Index += chunk;
			mp->submitted_bytes += chunk;
			if(node->Index >= node->Size) {
				node->StreamEnd = mp->submitted_bytes;
				node->StreamEndValid = TRUE;
			}
		}
		Permit();
		if(gen != mp->list_gen) return;   // drained under us: stop
		mhi_complete_consumed(mp, FALSE);
	}
}

// Open the SDK session (and, once, the shared buffers backing it).
static BOOL mhi_stream_open(struct MhiPlayer *mp) {
	ZZ9KAudioStreamBeginDesc begin;
	struct zz_mhi_transport_geometry geometry =
		zz_mhi_transport_geometry(mp->zorro_version);

	if(mp->session != 0) return TRUE;

	if(!mp->rings_allocated) {
		// The staging buffer is the only one the 68k writes, so it is
		// the only one that must be CPU-mappable. HOST_WINDOW places it
		// in the firmware's board-window heap on Zorro 2 (the library
		// strips the flag on Zorro 3, and pre-host-window-heap firmware
		// ignores it -- the alloc then fails to map on Z2 exactly like
		// a plain alloc did). The mp3/pcm rings live card-side only
		// (decode input, AX playback output): CARD_ONLY skips the
		// board-window mapping entirely, which is what makes them
		// allocatable on Z2 in the first place.
		if(ZZ9KAllocShared(geometry.staging_bytes, 16,
		                   ZZ9K_ALLOC_HOST_WINDOW,
		                   &mp->staging) != ZZ9K_STATUS_OK) {
			KPrintF("stream_open: staging alloc failed\n");
			return FALSE;
		}
		if(ZZ9KAllocShared(geometry.mp3_ring_bytes, 16,
		                   ZZ9K_ALLOC_CARD_ONLY,
		                   &mp->mp3_ring) != ZZ9K_STATUS_OK) {
			KPrintF("stream_open: mp3 ring alloc failed\n");
			ZZ9KFreeShared(mp->staging.handle);
			return FALSE;
		}
		if(ZZ9KAllocShared(geometry.pcm_ring_bytes, 16,
		                   ZZ9K_ALLOC_CARD_ONLY,
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
	        geometry.pcm_low_water_bytes, 0, 0)) {
		KPrintF("stream_open: begin desc rejected (client side)\n");
		return FALSE;
	}
	if(ZZ9KAudioStreamBegin(&begin, &mp->result) != ZZ9K_STATUS_OK) {
		KPrintF("stream_open: BEGIN rejected\n");
		return FALSE;
	}

	mp->session = mp->result.session;
	mp->backpressure = FALSE;
	mp->eof_announced = FALSE;
	mp->drain_requested = FALSE;
	mp->starvation_polls = 0;
	mp->submitted_bytes = 0;
	mp->feeds_accepted = 0;
	KPrintF("stream geometry: staging=%lu mp3=%lu pcm=%lu low=%lu\n",
	        (ULONG)geometry.staging_bytes, (ULONG)geometry.mp3_ring_bytes,
	        (ULONG)geometry.pcm_ring_bytes,
	        (ULONG)geometry.pcm_low_water_bytes);
	KPrintF("stream_open: session=%lu\n", (ULONG)mp->session);
	return TRUE;
}

// Stop AX playback and close the session (Stop/Free semantics; Pause
// keeps the session so PLAY resumes gaplessly). Task context only.
static void mhi_stream_close(struct MhiPlayer *mp) {
	ZZ9KAudioStreamResult r;
	int tries;
	int rc = ZZ9K_STATUS_BUSY;

	if(mp->session == 0) return;
	(void)ZZ9KAudioStreamStop(mp->session, 0, &r);
	// The firmware answers BUSY while an internal PCM-refill for this
	// session is still in flight on the card's second core (freeing
	// the session under it would corrupt decoder state); it drains
	// within milliseconds, so retry rather than leak the session.
	for(tries = 25; tries > 0; tries--) {
		rc = ZZ9KAudioStreamClose(mp->session, 0, &r);
		if(rc != ZZ9K_STATUS_BUSY)
			break;
		Delay(1);
	}
	mp->session = 0;
	mp->staged_valid = FALSE;
	mp->play_pending = FALSE;
	mp->eof_announced = FALSE;
	mp->drain_requested = FALSE;
	mp->starvation_polls = 0;
	mp->submitted_bytes = 0;
	if(rc != ZZ9K_STATUS_OK && rc != ZZ9K_STATUS_BAD_HANDLE) {
		// The close was NOT confirmed: either BUSY never drained (a
		// wedged card) or the transport failed (TIMEOUT/IO_ERROR/...).
		// Only OK means the card released the session, and BAD_HANDLE
		// means it is already gone; any other result leaves the card
		// potentially still owning this session and writing its PCM/mp3
		// rings. Freeing those rings (FreeDecoder) would be a card-side
		// use-after-free, and rebinding them to a new session
		// (mhi_stream_open reuses rings while rings_allocated) would
		// corrupt the in-flight decode. Abandon the rings instead:
		// clearing rings_allocated makes the next open allocate a fresh
		// set and makes FreeDecoder leave these alone. Leaking one ring
		// set on an already-wedged card is the safe failure.
		KPrintF("stream_close: close not confirmed (rc=%ld); "
		        "abandoning rings\n", (LONG)rc);
		mp->rings_allocated = FALSE;
	}
}

#ifdef ZZ_MHI_DIAG_DECODE_ONLY
/* Diagnostic discriminator: keep the MHI feeder and firmware decoder busy
 * while deliberately leaving the AX pump unbound. Retire decoded PCM through
 * READ so the small MHI rings cannot fill and accidentally turn the test idle.
 * This build is silent by design and must never replace the production target. */
static void mhi_diag_consume_pcm(struct MhiPlayer *mp) {
	ULONG used;

	if(!mp || mp->session == 0) return;
	used = mp->result.pcm_write - mp->result.pcm_read;
	if(used != 0)
		(void)ZZ9KAudioStreamRead(mp->session, used, 0, &mp->result);
}
#endif

// Complete a deferred Play: bind the session to the AX output once the
// card has decoded PCM and knows the sample rate. MHIPlay may legally
// arrive BEFORE any data is queued (the legacy driver allowed it, and
// seeking apps rely on it: Stop, Play, then requeue from the new file
// position), so Play sets play_pending and the bind happens here, from
// whichever entry point feeds the decisive chunk. Task context only.
static void mhi_try_bind(struct MhiPlayer *mp) {
	if(!mp->play_pending || mp->session == 0) return;
	// A pause can land between Play and the card reporting decoded
	// PCM; play_pending stays armed for the resume, but audio must
	// not start while the public state is PAUSED.
	if(mp->Status != MHIF_PLAYING) return;
	if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) == 0 ||
	   mp->result.sample_rate == 0) {
		// The cached FEED result can be stale: once every queued chunk
		// has been accepted there is no further FEED to refresh
		// mp->result, so PCM readiness the card reaches after that last
		// FEED would never be observed and play_pending would stick
		// forever (MHIF_PLAYING reported, no audio). Probe the live
		// state with a zero-length READ -- it consumes no PCM and does
		// not bind, but re-runs the decoder and returns fresh
		// flags/sample_rate. (A plain zero-length FEED is not an option:
		// the SDK builder accepts one only for explicit EOF or drain.)
		if(ZZ9KAudioStreamRead(mp->session, 0, 0, &mp->result)
		   != ZZ9K_STATUS_OK)
			return;
		if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_PCM_READY) == 0 ||
		   mp->result.sample_rate == 0)
			return;
	}
#ifdef ZZ_MHI_DIAG_DECODE_ONLY
	mp->play_pending = FALSE;
	KPrintF("MHI diagnostic: decoder active, AX binding suppressed.\n");
	return;
#endif
	if(ZZ9KAudioStreamPlay(mp->session, 0, &mp->result) != ZZ9K_STATUS_OK) {
		KPrintF("mhi_try_bind: PLAY rejected.\n");
		return;
	}
	// A Stop/Pause that raced the mailbox call above already unbound
	// the session card-side; nothing to undo here.
	mp->play_pending = FALSE;
	KPrintF("mhi_try_bind: session bound to AX output.\n");
}

// io_lock must be held. Only the explicit zero-length zzplay extension ends
// the stream permanently; public MHI starvation uses the resumable path below.
static BOOL mhi_stream_feed_eof_locked(struct MhiPlayer *mp) {
	ZZ9KAudioStreamFeedDesc feed;

	if(!mp || mp->session == 0 || mp->have_unfed ||
	   mp->feeds_accepted == 0)
		return FALSE;
	if(mp->eof_announced)
		return TRUE;
	if(!zz9k_audio_build_stream_feed_desc(
	       &feed, mp->session, 0, 0, 0,
	       ZZ9K_AUDIO_STREAM_FEED_EOF) ||
	   ZZ9KAudioStreamFeed(&feed, &mp->result) != ZZ9K_STATUS_OK ||
	   (mp->result.flags &
	    ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0)
		return FALSE;

	mp->eof_announced = TRUE;
	mp->drain_requested = FALSE;
	mp->starvation_polls = 0;
	mhi_complete_consumed(mp, FALSE);
	// A short file may not have produced enough PCM to complete the deferred
	// bind until EOF relaxed the decoder's minimum-input gate.
	mhi_try_bind(mp);
	KPrintF("MHI: EOF accepted\n");
	return TRUE;
}

// mhizz9000 EOF extension: a zero-length MHIQueueBuffer call means that the
// client has reclaimed every real buffer and will not queue more data for
// this play. The BOOL return makes it self-detecting: an older driver rejects
// the request. Standard MHI clients remain resumable through queue starvation.
static BOOL mhi_stream_eof(struct MhiPlayer *mp) {
	BOOL accepted = FALSE;
	BOOL can_feed = FALSE;
	UBYTE old_status = MHIF_STOPPED;
	ULONG transport_gen = 0;

	if(!mp) return FALSE;
	ObtainSemaphore(&mp->io_lock);
	Forbid();
	old_status = mp->Status;
	if(mp->session != 0 && !mp->have_unfed &&
	   (old_status == MHIF_PLAYING || old_status == MHIF_OUT_OF_DATA)) {
		transport_gen = mp->transport_gen;
		// A preceding status poll may have latched a temporary starvation
		// before the client reclaimed its final buffer. Re-enter PLAYING so
		// the explicit EOF can decode/drain that tail and be polled again.
		if(old_status == MHIF_OUT_OF_DATA)
			mp->Status = MHIF_PLAYING;
		can_feed = TRUE;
	}
	Permit();
	if(can_feed)
		accepted = mhi_stream_feed_eof_locked(mp);
	if(!accepted && old_status == MHIF_OUT_OF_DATA) {
		// Restore the pre-call status only if Stop/Free did not win while
		// the mailbox call was in flight.
		Forbid();
		if(mp->transport_gen == transport_gen &&
		   mp->Status == MHIF_PLAYING)
			mp->Status = MHIF_OUT_OF_DATA;
		Permit();
	}
	ReleaseSemaphore(&mp->io_lock);
	if(accepted) mhi_wake_feeder(mp);
	return accepted;
}

// io_lock must be held. Request a resumable starvation drain, never permanent
// EOF: the firmware retains any incomplete compressed frame and clears the
// drain automatically when later input is accepted.
static BOOL mhi_stream_feed_drain_locked(struct MhiPlayer *mp) {
	ZZ9KAudioStreamFeedDesc feed;
	BOOL accepted = FALSE;

	if(!mp || mp->session == 0 || mp->have_unfed ||
	   mp->feeds_accepted == 0)
		return FALSE;
	if(mp->drain_requested)
		return TRUE;
	if(!zz9k_audio_build_stream_feed_desc(
	       &feed, mp->session, 0, 0, 0,
	       ZZ9K_AUDIO_STREAM_FEED_DRAIN) ||
	   ZZ9KAudioStreamFeed(&feed, &mp->result) != ZZ9K_STATUS_OK ||
	   (mp->result.flags &
	    ZZ9K_AUDIO_STREAM_RESULT_BACKPRESSURE) != 0)
		return FALSE;

	/* QueueBuffer can run while the mailbox call blocks. Do not overwrite
	 * its cancellation of local drain state: the queued non-empty FEED will
	 * clear the firmware drain and then mirror that state in mhi_feed_pending. */
	Forbid();
	if(!mp->have_unfed) {
		mp->drain_requested = TRUE;
		mp->starvation_polls = 0;
		accepted = TRUE;
	}
	Permit();
	mhi_complete_consumed(mp, FALSE);
	mhi_try_bind(mp);
	if(accepted) {
		KPrintF("MHI: resumable drain accepted\n");
	}
	return accepted;
}

static BOOL mhi_stream_status_retryable(int rc) {
	return rc == ZZ9K_STATUS_BUSY || rc == ZZ9K_STATUS_TIMEOUT;
}

// io_lock must be held. Once every queued byte is accepted, probe the live
// stream. Decoder consumption completes application buffers. Sustained
// NEED_INPUT requests a resumable drain; DRAINED means decoded PCM and the
// real AX DMA tail have retired, so public MHI may report OUT_OF_DATA while
// remaining ready to resume. Only zzplay's explicit extension waits for DONE.
static BOOL mhi_stream_service_drain(struct MhiPlayer *mp) {
	int rc;
	BOOL notify = FALSE;

	if(!mp || mp->session == 0 || mp->Status != MHIF_PLAYING ||
	   mp->have_unfed || mp->feeds_accepted == 0) {
		if(mp) mp->starvation_polls = 0;
		return FALSE;
	}

#ifdef ZZ_MHI_DIAG_DECODE_ONLY
	rc = ZZ9KAudioStreamRead(mp->session, 0, 0, &mp->result);
#else
	if(mp->play_pending)
		rc = ZZ9KAudioStreamRead(mp->session, 0, 0, &mp->result);
	else
		rc = ZZ9KAudioStreamPlay(mp->session, 0, &mp->result);
#endif
	if(rc != ZZ9K_STATUS_OK) {
		if(mhi_stream_status_retryable(rc))
			return TRUE;
		/* BAD_HANDLE/BAD_REQUEST/UNSUPPORTED/IO_ERROR/INTERNAL_ERROR and
		 * other terminal replies cannot make progress by polling forever. */
		Forbid();
		if(mp->Status == MHIF_PLAYING) {
			mp->Status = MHIF_STOPPED;
			mp->play_pending = FALSE;
			notify = TRUE;
		}
		Permit();
		KPrintF("MHI: stream status failed (rc=%ld)\n", (LONG)rc);
		if(notify) mhi_signal_app(mp);
		return FALSE;
	}
	mhi_complete_consumed(mp, FALSE);

	if(mp->eof_announced) {
		if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DONE) == 0)
			return TRUE;
	} else {
		if(!mp->drain_requested) {
			if((mp->result.flags &
			    ZZ9K_AUDIO_STREAM_RESULT_NEED_INPUT) == 0) {
				mp->starvation_polls = 0;
				return TRUE;   // data/PCM still draining toward NEED_INPUT
			}
			if(mp->starvation_polls < ZZ_MHI_DRAIN_IDLE_POLLS)
				mp->starvation_polls++;
			if(mp->starvation_polls < ZZ_MHI_DRAIN_IDLE_POLLS)
				return TRUE;
			if(!mhi_stream_feed_drain_locked(mp))
				return TRUE;
		}
		if((mp->result.flags & ZZ9K_AUDIO_STREAM_RESULT_DRAINED) == 0)
			return TRUE;
		mhi_complete_consumed(mp, TRUE);
	}

	Forbid();
	if(mp->Status == MHIF_PLAYING && !mp->have_unfed) {
		mp->Status = MHIF_OUT_OF_DATA;
		notify = TRUE;
	}
	Permit();
	if(notify) {
		KPrintF("MHI: OUT_OF_DATA\n");
		mhi_signal_app(mp);
	}
	return FALSE;
}

/* ********************* */
/*  BEGIN feeder process */
/* ********************* */

// Retry cadence while the card is backpressured or a deferred Play is
// waiting. Base 50 ms, doubling per fruitless retry up to the cap and
// resetting whenever a FEED is accepted (or a wake arrives): the card
// only frees chunk-sized space every couple hundred ms, so most fixed
// 50 ms retries were wasted mailbox ops. The decoded PCM ring holds
// ~350 ms, which cushions even a capped sleep; with the staging memo
// a retry is one mailbox op.
#define ZZ_MHI_FEEDER_RETRY_MICROS     50000UL
#define ZZ_MHI_FEEDER_RETRY_MAX_MICROS 200000UL

// Single decoder (enforced by NumAllocatedDecoders), so the feeder
// entry point can pick up its MhiPlayer through a static.
static struct MhiPlayer *g_feeder_mp;

// Wake the feeder (new data queued, Play issued, teardown). Forbid
// pins the task pointer against feeder exit.
static void mhi_wake_feeder(struct MhiPlayer *mp) {
	Forbid();
	if(mp->feeder_task) Signal(mp->feeder_task, mp->feeder_wake_mask);
	Permit();
}

// The driver's own feed engine context. MHI applications are allowed
// to sleep until a buffer-completion signal, so the driver must make
// feed progress on its own: the legacy driver did it from the
// per-period interrupt; our mailbox calls block, so it happens here,
// in a dedicated process. All feed-engine and session mailbox activity
// is serialized with the control entry points through mp->io_lock.
static void mhi_feeder(void) {
	struct MhiPlayer *mp = g_feeder_mp;
	struct MsgPort *port = NULL;
	struct timerequest *treq = NULL;
	BYTE sig = -1;
	int dev_open = 0;

	if(!mp) return;

	sig = AllocSignal(-1);
	port = CreateMsgPort();
	if(sig < 0 || !port) goto out;
	treq = (struct timerequest *)CreateIORequest(port,
	                                             sizeof(struct timerequest));
	if(!treq) goto out;
	/* Retry deadlines are real microsecond timers. The vblank timer unit
	 * hooks the feeder into vertical-blank service while MHI is active; this
	 * was the remaining difference from the stable ZZPlay/AHI timer path. */
	if(OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
	              (struct IORequest *)treq, 0) != 0) goto out;
	dev_open = 1;

	mp->feeder_wake_mask = 1UL << sig;
	Forbid();
	mp->feeder_task = FindTask(NULL);
	mp->feeder_state = 1;
	Permit();
	KPrintF("feeder: running\n");

	{
	ULONG retry_micros = ZZ_MHI_FEEDER_RETRY_MICROS;

	for(;;) {
		BOOL busy = FALSE;
		BOOL drain_busy = FALSE;
		ULONG accepted_before = mp->feeds_accepted;
		ULONG sigs;

		ObtainSemaphore(&mp->io_lock);
		// Feed ONLY while PLAYING. MHIPause unbinds the AX output but keeps
		// the session open (Status == MHIF_PAUSED), and mhi_feed_pending()
		// marks a fully-handed buffer Played and signals the app -- and MHI
		// apps advance their elapsed-time accounting per completion signal.
		// Feeding while paused would therefore let the app clock and reclaim
		// buffers with no audio playing, unlike the legacy interrupt path
		// which stopped buffer-completion progress at Pause. Resume flips
		// Status back to MHIF_PLAYING before waking us, so nothing is lost.
		if(!mp->feeder_quit && mp->session != 0 &&
		   mp->Status == MHIF_PLAYING) {
			mhi_feed_pending(mp);
			mhi_try_bind(mp);
#ifdef ZZ_MHI_DIAG_DECODE_ONLY
			mhi_diag_consume_pcm(mp);
#endif
			drain_busy = mhi_stream_service_drain(mp);
			busy = mp->have_unfed || mp->backpressure ||
			       mp->play_pending || drain_busy;
		}
		ReleaseSemaphore(&mp->io_lock);

		if(mp->feeder_quit) break;

		if(busy) {
			// Adaptive pacing: back off while the card refuses,
			// snap back on progress.
			if(mp->feeds_accepted != accepted_before) {
				retry_micros = ZZ_MHI_FEEDER_RETRY_MICROS;
			} else {
				retry_micros <<= 1;
				if(retry_micros > ZZ_MHI_FEEDER_RETRY_MAX_MICROS)
					retry_micros = ZZ_MHI_FEEDER_RETRY_MAX_MICROS;
			}
			// Timed wait: retry even without a wake signal.
			treq->tr_node.io_Command = TR_ADDREQUEST;
			treq->tr_time.tv_secs = 0;
			treq->tr_time.tv_micro = retry_micros;
			SendIO((struct IORequest *)treq);
			sigs = Wait(mp->feeder_wake_mask |
			            (1UL << port->mp_SigBit));
			if(!CheckIO((struct IORequest *)treq))
				AbortIO((struct IORequest *)treq);
			WaitIO((struct IORequest *)treq);
			// WaitIO can reap an already-replied message WITHOUT
			// consuming the port's signal bit (e.g. after a wake
			// aborted the timer). A stale bit makes every later
			// Wait return instantly -- the feeder then spins at
			// mailbox-op speed and pins the CPU (bench round 8).
			SetSignal(0, 1UL << port->mp_SigBit);
			if(sigs & mp->feeder_wake_mask)
				retry_micros = ZZ_MHI_FEEDER_RETRY_MICROS;
		} else {
			// Idle: sleep until something changes.
			retry_micros = ZZ_MHI_FEEDER_RETRY_MICROS;
			Wait(mp->feeder_wake_mask);
		}
	}
	}

out:
	KPrintF("feeder: exit\n");
	if(dev_open) CloseDevice((struct IORequest *)treq);
	if(treq) DeleteIORequest((struct IORequest *)treq);
	if(port) DeleteMsgPort(port);
	if(sig >= 0) FreeSignal(sig);
	// Single final mp access: AllocDecoder frees the player the moment
	// it observes feeder_state == 2, and FreeDecoder the moment it
	// observes feeder_task == NULL -- so whichever exit this is, its
	// publication must be the LAST thing that touches mp (publishing
	// failure before the resource cleanup above was a use-after-free
	// window on the startup-failure path).
	Forbid();
	if(mp->feeder_state == 1) {
		mp->feeder_task = NULL;   // normal exit (quit requested)
	} else {
		mp->feeder_state = 2;     // startup failure; task was never set
	}
	Permit();
}

/* ******************* */
/*  END feeder process */
/* ******************* */

/* ******************* */
/*  END feed engine    */
/* ******************* */

extern ULONG dev_sisr(struct MhiPlayer *mp asm("a1"));
ULONG cdev_sisr(struct MhiPlayer *mp asm("a1")) {
	// Deliberately inert: mailbox feed/drain calls block and therefore belong
	// to the dedicated feeder process, which also publishes completion and
	// OUT_OF_DATA signals at the appropriate state transitions. The interrupt
	// machinery remains installed because its shared-server-list node is the
	// MHI-vs-AHI ownership token.
	return 0;
}

extern ULONG dev_isr(struct MhiPlayer *mp asm("a1"));
extern ULONG dev_token_isr(void);
ULONG cdev_isr(struct MhiPlayer *mp asm("a1")) {
	// Mailbox feeder progress does not use the hardware audio IRQ. Keep
	// this server inert: when fabric AHI records concurrently, reading or
	// acknowledging the shared interrupt here can steal its capture wake.
	(void)mp;
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

	// Keep the real node visible to pre-fabric AHI binaries for the
	// decoder's full lifetime. A qualified fabric sentinel is added only
	// after the capability checks below have succeeded.
	mp->irq.is_Node.ln_Type = NT_INTERRUPT;
	mp->irq.is_Node.ln_Pri  = 126; // High priority: this ISR must react quickly.
	mp->irq.is_Node.ln_Name = ZZ_AX_IRQ_NAME_MHI;
	mp->irq.is_Data = mp;
	mp->irq.is_Code = (void*)dev_isr;

	mp->irq_fabric_token.is_Node.ln_Type = NT_INTERRUPT;
	mp->irq_fabric_token.is_Node.ln_Pri = 125;
	mp->irq_fabric_token.is_Node.ln_Name = ZZ_AX_IRQ_NAME_MHI_FABRIC;
	mp->irq_fabric_token.is_Data = mp;
	mp->irq_fabric_token.is_Code = (void*)dev_token_isr;
}

// Install the hard interrupt server. MUST be called under Forbid() so the
// caller can combine it with ahi_mode_locked() atomically.
static void install_irq_server_locked(struct MhiPlayer *mp) {
	if (mp->flags & ZZ_AX_DEVF_INT2MODE) {
		AddIntServer(INTB_PORTS, &mp->irq);
	} else {
		AddIntServer(INTB_EXTER, &mp->irq);
	}
}

// Keep the HW-side audio interrupt OFF when MHI is the only owner.
// With a fabric AHI peer the shared register belongs to AHI's playback/
// capture directions, so Stop/Pause/Free must not clear it. MUST run
// under Forbid() so the IRQ-name ownership token cannot change mid-check.
static void disable_hw_audio(struct MhiPlayer *mp) {
	if(ahi_mode_locked(mp->flags) != MHI_AHI_MODE_FABRIC)
		setRegister(mp, ZZ_REG_AUDIO_CONFIG, 0);
}

// Undo the atomic ownership claim in i_MHIAllocDecoder (the hard-IRQ
// server, the decoder count, and the published library base) when a later
// step of the same allocation fails. MUST run under Forbid(), mirroring
// the claim window. The caller still frees mp/BufferList and closes its
// own library reference.
static void unclaim_ownership_locked(struct MhiPlayer *mp,
                                     struct MHI_LibBase *MHI_LibBase) {
	if (mp->flags & ZZ_AX_DEVF_INT2MODE) {
		if(mp->fabric_token_installed)
			RemIntServer(INTB_PORTS, &mp->irq_fabric_token);
		RemIntServer(INTB_PORTS, &mp->irq);
	} else {
		if(mp->fabric_token_installed)
			RemIntServer(INTB_EXTER, &mp->irq_fabric_token);
		RemIntServer(INTB_EXTER, &mp->irq);
	}
	mp->fabric_token_installed = FALSE;
	if(MHI_LibBase->NumAllocatedDecoders)
		MHI_LibBase->NumAllocatedDecoders--;
	ZZ9KBase = NULL;   // unpublish with the ownership release
}

/*
 *
 */
APTR i_MHIAllocDecoder(REGA0(struct Task *mhi_task), REGD0(ULONG mhi_sigmask), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = NULL;
	struct Library *base;

	// The modern decoder path runs through zz9k.library audio-stream
	// sessions plus the firmware's AX playback binding; require both.
	// Every attempt takes ITS OWN OpenLibrary reference (library bases
	// are refcounted; the pointer is identical across opens) and
	// failure paths close only that reference. The global ZZ9KBase is
	// written solely by the claim winner inside the claim Forbid and
	// cleared by FreeDecoder inside the release Forbid, so no
	// concurrent allocate or teardown can close or NULL it under a
	// decoder that is using it.
	base = OpenLibrary((STRPTR)"zz9k.library", 0);
	if(!base) {
		KPrintF("Can't open zz9k.library.\n");
		return NULL;
	}
	// AUDIO_STREAM_DRAIN (rev 27) subsumes ALLOC_FLAGS (rev 26): this
	// driver passes ZZ9K_ALLOC_HOST_WINDOW/CARD_ONLY, and it is the
	// LIBRARY that strips HOST_WINDOW on Zorro 3. An older library
	// forwards the bit verbatim and new firmware would then place the
	// staging buffer inside Z3 P96 VRAM -- refuse the skew instead.
	// Revision 27 also accepts the resumable DRAIN request flag locally;
	// revision 26 would reject every starvation drain as BAD_REQUEST.
	if(base->lib_Revision <
	   ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_DRAIN) {
		KPrintF("zz9k.library too old for audio-stream drain.\n");
		CloseLibrary(base);
		return NULL;
	}

	mp = AllocVec(sizeof(struct MhiPlayer), MEMF_CLEAR);
	if(!mp) {
		KPrintF("Can't allocate MhiPlayer.\n");
		CloseLibrary(base);
		return NULL;
	}

	mp->hw_addr = MHI_LibBase->hw_addr;
	mp->flags   = MHI_LibBase->flags;
	mp->zorro_version = MHI_LibBase->zorro_version;

	mp->MhiTask = mhi_task;
	mp->MhiMask = mhi_sigmask;
	mp->Status  = MHIF_STOPPED;

	mp->volume  = 100;
	mp->panning = 50;

	mp->BufferList = AllocVec(sizeof(struct MinList), MEMF_PUBLIC|MEMF_CLEAR);
	if(!mp->BufferList) {
		FreeVec(mp);
		CloseLibrary(base);
		return NULL;
	}
	NewList((struct List *)mp->BufferList);
	InitSemaphore(&mp->io_lock);

	// Populate the Interrupt nodes so the atomic-claim step below can
	// AddIntServer our hard ISR directly.
	prepare_irq_structs(mp);

	// Atomic ownership claim: in one Forbid() window, verify no other MHI
	// decoder exists, classify any active AHI owner, then install our hard
	// IRQ token. This closes the TOCTOU that would otherwise let both
	// drivers decide independently whether coexistence is safe. The MHI
	// hard ISR is inert; mailbox feeder progress does not depend on it.
	UBYTE ahi_mode = MHI_AHI_MODE_NONE;
	Forbid();
	if(MHI_LibBase->NumAllocatedDecoders) {
		Permit();
		KPrintF("Can't allocate! Hardware already used by another MHI instance.\n");
		FreeVec(mp->BufferList);
		FreeVec(mp);
		CloseLibrary(base);
		return NULL;
	}
	// AHI's installed IRQ name is read inside the atomic claim. Only the
	// fabric-specific token permits coexistence; the legacy token covers
	// old binaries and current AHI instances whose fabric probe fell back.
	ahi_mode = ahi_mode_locked(MHI_LibBase->flags);
	install_irq_server_locked(mp);
	MHI_LibBase->NumAllocatedDecoders++;
	// Claim won: publish our library reference for the proto inlines.
	ZZ9KBase = base;
	Permit();

	// A current zz9k.library (revision-checked above) can still front a
	// firmware image that predates the AX playback op -- the binding lives
	// in firmware, not the library. Now that ZZ9KBase is live, ask the
	// running firmware what it advertises: without AUDIO_PLAYBACK the
	// MHIPlay stream bind returns UNSUPPORTED while the driver still reports
	// MHIF_PLAYING with no audio, so refuse the decoder here and let the app
	// fall back to Paula/AHI. The query blocks on the mailbox completion, so
	// it must run after the claim Permit, never under Forbid.
	{
		ZZ9KCaps caps;
		const ULONG required_caps =
			ZZ9K_CAP_AUDIO_PLAYBACK | ZZ9K_CAP_AUDIO_STREAM_DRAIN;
		if(ZZ9KQueryCaps(&caps) != ZZ9K_STATUS_OK ||
		   (caps.capability_bits & required_caps) != required_caps) {
			KPrintF("Firmware lacks matched MHI drain capability.\n");
			Forbid();
			unclaim_ownership_locked(mp, MHI_LibBase);
			Permit();
			FreeVec(mp->BufferList);
			FreeVec(mp);
			CloseLibrary(base);
			return NULL;
		}

		// R16 capability gate: the firmware-authoritative control plane
		// is deliberately unadvertised until qualified, so an absent
		// ZZ9K_CAP_AUDIO_CONTROL is the normal old-firmware case --
		// legacy playback with the old DSP stamps (LPF at Play start)
		// and no trim. Only a firmware that advertises the surface ever
		// hears from us as a control-plane client: remember the
		// capability for this decoder's lifetime (release trim, no
		// legacy stamps, scene-owned app mixer API) and submit this
		// owner's neutral source trim -- the pinned keep-baseline word,
		// "no trim from this owner" (R4). Like the query above, the
		// trim submission blocks on the mailbox completion and must
		// stay outside Forbid().
		if(caps.capability_bits & ZZ9K_CAP_AUDIO_CONTROL) {
			mp->audio_control_capped = TRUE;
			submit_source_trim();
		}
	}

	// AHI/MHI exclusion decision (AHI migration): the active AHI IRQ
	// token was observed inside the atomic claim above. Only the
	// fabric-specific token proves that this AHI instance is lease-backed;
	// a legacy token retains the classic exclusion.
	if(ahi_mode == MHI_AHI_MODE_LEGACY) {
		KPrintF("Can't allocate! Hardware already used by AHI.\n");
		Forbid();
		unclaim_ownership_locked(mp, MHI_LibBase);
		Permit();
		FreeVec(mp->BufferList);
		FreeVec(mp);
		CloseLibrary(base);
		return NULL;
	}

	// The legacy node kept concurrent and pre-fabric AHI claims fail-closed
	// while the blocking capability query ran, and remains installed for
	// mixed-version protection. Add the qualified inert sentinel under
	// Forbid so current AHI observes fabric compatibility atomically.
	Forbid();
	if (mp->flags & ZZ_AX_DEVF_INT2MODE)
		AddIntServer(INTB_PORTS, &mp->irq_fabric_token);
	else
		AddIntServer(INTB_EXTER, &mp->irq_fabric_token);
	mp->fabric_token_installed = TRUE;
	Permit();

	// No mixer stamp here (R4): the output balance is the firmware's to
	// set -- the operator baseline plus the trim submitted above -- and
	// a register write would be rejected by the scene authority gate on
	// matched firmware anyway.

	// Spawn the feeder process; it idles until Play opens a session.
	g_feeder_mp = mp;
	mp->feeder_state = 0;
	mp->feeder_quit = FALSE;
	// Normal priority: the Z3 batched rings provide ample runway, and running
	// this mailbox feeder above Intuition/graphics was measured to disturb
	// native-overlay presentation while an MHI stream was active.
	if(!CreateNewProcTags(NP_Entry, (ULONG)mhi_feeder,
	                      NP_Name, (ULONG)"mhizz9000 feeder",
	                      NP_StackSize, 16384,
	                      NP_Priority, ZZ_MHI_FEEDER_PRIORITY,
	                      TAG_DONE)) {
		mp->feeder_state = 2;
	}
	while(mp->feeder_state == 0) Delay(1);
	if(mp->feeder_state != 1) {
		KPrintF("Can't start the feeder process.\n");
		Forbid();
		unclaim_ownership_locked(mp, MHI_LibBase);
		Permit();
		FreeVec(mp->BufferList);
		FreeVec(mp);
		CloseLibrary(base);
		return NULL;
	}

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
	// in-flight MMIO during teardown.
	Forbid();
	mp->Status = MHIF_STOPPED;
	mp->play_pending = FALSE;
	mp->pause_pending = FALSE;
	mp->transport_gen++;
	disable_hw_audio(mp);
	drain_buffer_list_locked(mp);
	Permit();

	// Retire the feeder before touching the session: after this no
	// other context issues mailbox calls on mp.
	if(mp->feeder_task) {
		mp->feeder_quit = TRUE;
		mhi_wake_feeder(mp);
		while(mp->feeder_task) Delay(1);
	}
	// Permit-to-0 dispatches any queued soft IRQ while mp is still valid.
	// No new soft IRQ can be caused: MHI's installed hard ISR is inert and
	// exists only as the ownership token.

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

	// Release the control-plane trim claimed at allocate: submit the
	// reserved neutral balance word -- the pinned keep-baseline
	// release, "no trim from this owner"; the firmware answers with
	// the operator baseline pair and does not restage the mixer. No
	// other DSP state is rewritten on release (R4): the master chain
	// (volume, prefactor, EQ) was never ours to touch, and register
	// writes for those params are rejected by the firmware authority
	// gate regardless. The submission blocks on the mailbox
	// completion, so it runs here -- outside Forbid and while
	// ZZ9KBase is still published.
	if(mp->audio_control_capped) submit_source_trim();

	// --- Phase 3: atomically release ownership. ---
	// Only now do we give up the card: remove both ownership nodes, decrement
	// NumAllocatedDecoders AND unpublish ZZ9KBase inside a single Forbid
	// window so any racing AllocDecoder / AHI AllocAudio sees all state
	// changes together. Our library reference is captured and closed
	// AFTER the release: a racing winner publishes its own reference
	// (same base pointer, own refcount), so it never dispatches through
	// a base this close could invalidate.
	{
		struct Library *base;

		Forbid();
		if (mp->flags & ZZ_AX_DEVF_INT2MODE) {
			if(mp->fabric_token_installed)
				RemIntServer(INTB_PORTS, &mp->irq_fabric_token);
			RemIntServer(INTB_PORTS, &mp->irq);
		} else {
			if(mp->fabric_token_installed)
				RemIntServer(INTB_EXTER, &mp->irq_fabric_token);
			RemIntServer(INTB_EXTER, &mp->irq);
		}
		mp->fabric_token_installed = FALSE;
		if(MHI_LibBase->NumAllocatedDecoders) MHI_LibBase->NumAllocatedDecoders--;
		base = ZZ9KBase;
		ZZ9KBase = NULL;
		Permit();

		// --- Phase 4: free memory after ownership is released. ---
		if(mp->BufferList) {
			FreeVec(mp->BufferList);
			mp->BufferList = NULL;
		}
		FreeVec(mp);

		if(base) CloseLibrary(base);
	}
}


/*
 *
 */
BOOL i_MHIQueueBuffer(REGA3(APTR mhi_handle), REGA0(APTR mhi_buffer), REGD0(ULONG mhi_size), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;

	if(mp == NULL || mp->BufferList == NULL) {
		return FALSE;
	}
	if(mhi_size == 0)
		return mhi_stream_eof(mp);
	if(!mhi_buffer)
		return FALSE;

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
	BufferNode->StreamEnd = 0;
	BufferNode->StreamEndValid = FALSE;
	BufferNode->Played = FALSE;

	// Forbid() while linking: a feed snapshot in another task must not
	// observe a half-linked tail.
	Forbid();
	AddTail((struct List *)mp->BufferList, (struct Node *)BufferNode);
	mp->have_unfed = TRUE;
	mp->drain_requested = FALSE;
	mp->starvation_polls = 0;
	// Auto-resume: new data after a drain (GetStatus recorded
	// MHIF_OUT_OF_DATA) returns the stream to PLAYING so the feeder --
	// gated on PLAYING -- picks it up and the still-bound pump plays it,
	// matching the legacy interrupt decoder that resumed on fresh data
	// without an explicit Play.
	if(mp->Status == MHIF_OUT_OF_DATA)
		mp->Status = MHIF_PLAYING;
	Permit();

	KPrintF("MHIQueueBuffer: Adr=0x%08lX Size=%lu\n", (ULONG)mhi_buffer,
	        (ULONG)mhi_size);

	// The feeder process moves the data; just wake it.
	if(mp->session != 0) {
		mhi_wake_feeder(mp);
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

	if(mhi_buffer) {
		KPrintF("MHIGetEmpty: Adr=0x%08lX\n", (ULONG)mhi_buffer);
	}

	return mhi_buffer;
}


/*
 *
 */
UBYTE i_MHIGetStatus(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(!mp) return MHIF_STOPPED;

	return mp->Status;
}


/*
 *
 */
void i_MHIPlay(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	KPrintF("MHIPlay called\n");

	if(!mp) return;

	switch(mp->Status) {
		case MHIF_STOPPED:
		case MHIF_OUT_OF_DATA: {
			ULONG gen;

			ObtainSemaphore(&mp->io_lock);
			Forbid();
			gen = mp->transport_gen;
			// Scope any pause intent to this open window: a stale
			// pause_pending from before this Play is cleared, while a
			// Pause that arrives during the blocking open below is
			// recorded and honored at the publish step.
			mp->pause_pending = FALSE;
			Permit();
			if(!mhi_stream_open(mp)) {
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: session open failed.\n");
				return;
			}

			// Anti-alias LPF at session start -- only when the
			// control-plane capability was absent at allocate: old
			// firmware has no scene module owning the cutoff, so the
			// legacy 20000 Hz stamp stands. On control-plane firmware
			// the active scene owns the low-pass cutoff and a
			// register write here would be rejected by the firmware
			// authority gate.
			if(!mp->audio_control_capped) {
				setAudioParam(mp, ZZ_AX_AP_DSP_SET_LOWPASS, 20000);
			}

			// Report PLAYING right away; the feeder process pushes
			// queued data and binds the session to the AX output as
			// soon as the card has decoded PCM and a sample rate.
			// Play may legally arrive with an empty (or too short)
			// queue -- notably on a seek, where the app stops, calls
			// Play, and requeues from the new file position -- so a
			// not-yet-ready card must not fail the call. A transport
			// command from another task that landed while the session
			// open was blocking wins over this Play: it saw STOPPED
			// and did nothing, so publishing PLAYING now would revive
			// a playback the app just cancelled.
			Forbid();
			if(mp->transport_gen != gen) {
				Permit();
				mhi_stream_close(mp);
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: cancelled by racing Stop.\n");
				return;
			}
			if(mp->pause_pending) {
				// A Pause raced this Play while it was blocked in
				// mhi_stream_open (Status was still STOPPED, so Pause
				// could not act). Come up PAUSED -- armed to resume via
				// the MHIF_PAUSED play_pending path -- instead of
				// starting audio behind the user's back.
				mp->pause_pending = FALSE;
				mp->Status = MHIF_PAUSED;
				mp->play_pending = TRUE;
				Permit();
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: paused by racing Pause.\n");
				return;
			}
			mp->Status = MHIF_PLAYING;
			mp->play_pending = TRUE;
			Permit();
			ReleaseSemaphore(&mp->io_lock);
			mhi_wake_feeder(mp);
		}
		break;
		case MHIF_PAUSED:
			if(mp->play_pending) {
				// Paused before the deferred bind completed: just
				// rearm; the feeder binds when data arrives.
				Forbid();
				mp->Status = MHIF_PLAYING;
				Permit();
				mhi_wake_feeder(mp);
				break;
			}
			// Resume: the session (and its PCM ring) survived Pause,
			// so re-binding continues gaplessly. A Stop can race between
			// the switch reading PAUSED above and this transition: it
			// flips Status to STOPPED under Forbid, then blocks on
			// io_lock to close the session. Only Stop moves PAUSED to a
			// non-PAUSED state, so re-check Status == MHIF_PAUSED under
			// Forbid both before the blocking rebind (skip a pointless
			// bind Stop is about to undo) and before publishing PLAYING
			// (do not overwrite STOPPED and strand the transport with a
			// closed session under MHIF_PLAYING, which has no MHIPlay
			// case to reopen). io_lock keeps our rebind ordered ahead of
			// Stop's close.
			ObtainSemaphore(&mp->io_lock);
			Forbid();
			if(mp->Status != MHIF_PAUSED) {
				Permit();
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: resume cancelled by racing Stop.\n");
				break;
			}
			Permit();
			if(ZZ9KAudioStreamPlay(mp->session, 0, &mp->result) != ZZ9K_STATUS_OK) {
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: resume rejected.\n");
				return;
			}
			Forbid();
			if(mp->Status != MHIF_PAUSED) {
				// Stop landed during the blocking rebind; it owns the
				// STOPPED state and will close the session under io_lock.
				Permit();
				ReleaseSemaphore(&mp->io_lock);
				KPrintF("MHIPlay: resume cancelled by racing Stop.\n");
				break;
			}
			mp->Status = MHIF_PLAYING;
			Permit();
			ReleaseSemaphore(&mp->io_lock);
			mhi_wake_feeder(mp);
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
	mp->pause_pending = FALSE;
	mp->transport_gen++;   // a Play blocked in session open must yield
	drain_buffer_list_locked(mp);
	mp->backpressure = FALSE;
	mp->drain_requested = FALSE;
	mp->starvation_polls = 0;
	Permit();

	if(old != MHIF_STOPPED) {
		// Stop resets the position: close the session entirely; the
		// next Play begins a fresh one (rings are reused). io_lock
		// waits out a feeder iteration that is mid-mailbox-call.
		ObtainSemaphore(&mp->io_lock);
		mhi_stream_close(mp);
		ReleaseSemaphore(&mp->io_lock);
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
	} else if(mp->Status == MHIF_STOPPED) {
		// A Play may be blocked in mhi_stream_open right now: Status is
		// still STOPPED until it publishes, so nothing to pause yet.
		// Record the intent so that Play comes up PAUSED instead of
		// starting audio. A stray pause with no Play in flight is cleared
		// when the next Play starts (see i_MHIPlay).
		mp->pause_pending = TRUE;
	}
	Permit();

	if(paused && mp->session != 0) {
		// Unbind from the AX output; the session and its decoded PCM
		// survive, so Play resumes exactly where we stopped.
		ObtainSemaphore(&mp->io_lock);
		if(mp->session != 0)
			(void)ZZ9KAudioStreamStop(mp->session, 0, &r);
		ReleaseSemaphore(&mp->io_lock);
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
#ifdef ZZ_MHI_DIAG_DECODE_ONLY
			return (ULONG)"ZZ9000AX (decode-only diagnostic)";
#else
			return (ULONG)"ZZ9000AX (SDK core-1)";
#endif

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

		// Mixer controls (volume, panning, prefactor, the EQ bands)
		// are deliberately NOT advertised: support is not universal,
		// and matched control-plane firmware rejects the corresponding
		// setters (see i_MHISetParam). Advertising them would make
		// compliant players expose dead controls. Direct MHISetParam
		// calls still work against pre-control-plane firmware.
		case MHIQ_VOLUME_CONTROL:
		case MHIQ_PREFACTOR_CONTROL:
		case MHIQ_BASS_CONTROL:
		case MHIQ_TREBLE_CONTROL:
		case MHIQ_MID_CONTROL:
		case MHIQ_5_BAND_EQ:
		case MHIQ_10_BAND_EQ:
			return MHIF_UNSUPPORTED;


		default:
			return MHIF_UNSUPPORTED;
	}
}

/*
 * App mixer API. The master-chain parameters (volume/panning,
 * prefactor, the EQ bands) are legacy-only, and i_MHIQuery therefore
 * does not advertise them: support is not universal, and matched
 * control-plane firmware returns unsupported for these setters. They
 * map straight onto master-chain DSP registers that the scene module
 * owns once the firmware advertised the control plane at allocate. In
 * that case the register write is skipped -- the scene authority gate
 * would reject it anyway -- and the documented not-supported status
 * is reported; use scenes (ZZTop's Audio window) on control-plane
 * firmware. Against pre-control-plane firmware the direct writes
 * stand, so legacy callers that invoke MHISetParam directly (without
 * probing MHIQuery) keep working.
 */
ULONG i_MHISetParam(REGA3(APTR mhi_handle), REGD0(UWORD mhi_param), REGD1(ULONG mhi_value), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(mp) {
		switch(mhi_param) {
			case MHIP_PANNING:
			case MHIP_VOLUME:
			case MHIP_PREFACTOR:
			case MHIP_BAND1:
			case MHIP_BAND2:
			case MHIP_BAND3:
			case MHIP_BAND4:
			case MHIP_BAND5:
			case MHIP_BAND6:
			case MHIP_BAND7:
			case MHIP_BAND8:
			case MHIP_BAND9:
			case MHIP_BAND10:
				if(mp->audio_control_capped) {
					KPrintF("MHISetParam: %ld is scene-owned on control-plane firmware.\n", (ULONG)mhi_param);
					return MHIF_UNSUPPORTED;
				}
				break;
			default:
				break;
		}
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
	return MHIF_SUPPORTED;
}
