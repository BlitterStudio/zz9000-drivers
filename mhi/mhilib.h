#ifndef MHILIB_H
#define MHILIB_H

/* MHI status flags for player */
// #define MHIF_PLAYING        0
// #define MHIF_STOPPED        1
// #define MHIF_OUT_OF_DATA    2
// #define MHIF_PAUSED         3

#include "zz9k/audio.h"
#include "zz9k/host.h"

struct MHI_LibBase {
	struct Library mhi_Library;
	BPTR mhi_SegList;
	struct ExecBase *mhi_SysBase;
	struct DosLibrary *mhi_DOSBase;
	ULONG NumAllocatedDecoders;

	ULONG hw_addr;
	ULONG hw_size;

	UBYTE zorro_version;
	UBYTE flags;
};

struct MhiPlayer {
	struct Task *MhiTask;
	ULONG MhiMask;
	struct MinList *BufferList;
	ULONG Status;

	struct Interrupt irq;
	struct Interrupt sirq;

	ULONG hw_addr;

	UBYTE flags;
	UBYTE zorro_version;
	UBYTE volume;
	UBYTE panning;

	/*
	 * SDK audio-stream session state. Decode runs on the card's second
	 * CPU core; the firmware's AX playback pump feeds the audio DMA
	 * straight from the session's PCM ring, so PCM never crosses Zorro
	 * and this driver does no per-period work at all.
	 */
	ULONG session;               /* 0 = no session open */
	ZZ9KSharedBuffer staging;    /* 68k -> card chunk transport */
	ZZ9KSharedBuffer mp3_ring;   /* card-side compressed ring */
	ZZ9KSharedBuffer pcm_ring;   /* card-side PCM ring (pump-consumed) */
	ZZ9KAudioStreamResult result;
	UBYTE rings_allocated;
	UBYTE backpressure;          /* card mp3 ring full; retry later */
	UBYTE have_unfed;            /* queued app data not yet on the card */
	UBYTE staged_valid;          /* staging holds the chunk keyed below */
	ULONG list_gen;              /* bumped on drain: aborts in-flight feeds */

	/*
	 * Backpressure-retry memo: while the card refuses a chunk, the bytes
	 * already sit in the staging buffer, so a retry skips the 68k->card
	 * copy and only repeats the cheap FEED op. Keyed on (gen, node,
	 * index, chunk); invalidated on every accepted FEED so a recycled
	 * ListNode allocation can never alias a stale key.
	 */
	APTR  staged_node;
	ULONG staged_index;
	ULONG staged_chunk;
	ULONG staged_gen;

	UBYTE play_pending;          /* PLAYING, but AX bind deferred until
	                                the card has decoded PCM + rate */

	/*
	 * The feed engine runs in a driver-owned feeder process, NOT in the
	 * application's context: mailbox calls block, so they can't run
	 * from interrupts -- and MHI apps (AmigaAMP) sleep until a
	 * completion signal, so feeding can't ride their entry points
	 * either (the first buffer would never complete if it exceeds the
	 * card-side rings). io_lock serializes the feeder against the
	 * control entry points (Play/Stop/Pause/GetStatus probe).
	 */
	struct SignalSemaphore io_lock;
	struct Task *feeder_task;    /* NULL once the feeder has exited */
	ULONG feeder_wake_mask;
	volatile UBYTE feeder_state; /* 0 starting, 1 running, 2 failed */
	volatile UBYTE feeder_quit;
};

struct ListNode {
	struct MinNode Header;
	UBYTE*         Buffer;
	ULONG          Size;
	ULONG          Index;
	BOOL           Played;
};

#endif
