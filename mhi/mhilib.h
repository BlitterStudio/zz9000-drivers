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
	UBYTE pad0;
	ULONG list_gen;              /* bumped on drain: aborts in-flight feeds */
};

struct ListNode {
	struct MinNode Header;
	UBYTE*         Buffer;
	ULONG          Size;
	ULONG          Index;
	BOOL           Played;
};

#endif
