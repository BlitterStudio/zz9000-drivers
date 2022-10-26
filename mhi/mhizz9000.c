
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

// Comment out to enable debug output:
#define KPrintF(...)

#define DEVF_INT2MODE 1

#define ZZ_BYTES_PER_PERIOD 3840
#define AUDIO_BUFSZ ZZ_BYTES_PER_PERIOD*8 // TODO: query from hardware
#define WORKER_PRIORITY 127 // 19 would be nicer

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

#define FIFOSIZE (1152*4 + 1)
//#define FIFOSIZE (16*1024+1)

typedef enum {
	DECODE_CLEAR,
	DECODE_INIT,
	DECODE_RUN
} DECODE_COMMAND;

#define BSWAP_S(x) ((UWORD) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define BSWAP_L(x) (((((ULONG)x) & 0xff000000u) >> 24) | ((((ULONG)x) & 0x00ff0000u) >> 8) | ((((ULONG)x) & 0x0000ff00u) << 8) | ((((ULONG)x) & 0x000000ffu) << 24))
#define BSWAP_P(x) (void*)(((((ULONG)x) & 0xff000000u) >> 24) | ((((ULONG)x) & 0x00ff0000u) >> 8) | ((((ULONG)x) & 0x0000ff00u) << 8) | ((((ULONG)x) & 0x000000ffu) << 24))

/* ************ */
/*  BEGIN FIFO  */
/* ************ */
// Clear FIFO on both sides.
static void clearFifo(struct MhiPlayer *mp) {
	mp->FifoMode = FIFO_PREFILL;
	mp->FifoWriteIdx = 0;
	// ZZ_DECODE (clear)
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODE)) = DECODE_CLEAR;
}

static void fillFifo(struct MhiPlayer *mp) {
	UBYTE *Buffer = (UBYTE *)mp->mp3_addr;
	LONG Space = 0;
	UWORD FifoReadIdx;
	struct ListNode *BufferNode;
	LONG i;

	// 1. Get FIFO Read Index from ZZ9k (we are the slave).
	FifoReadIdx = *((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_FIFO));
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
	*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_FIFO)) = mp->FifoWriteIdx;
}
/* ********** */
/*  END FIFO  */
/* ********** */

BOOL UserLibInit(struct MHI_LibBase *MhiLibBase) {
	struct ConfigDev* cd;
	ULONG hw_addr = 0;
	int ax_present;

	MhiLibBase->zorro_version = 0;
	if((ExpansionBase = (struct ExpansionBase*) OpenLibrary((STRPTR)"expansion.library", 0))) {
		// Find Z2 or Z3 model of MNT ZZ9000
		if((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6D6E, 0x4))) {
			// ZORRO 3
			MhiLibBase->zorro_version = 3;
			KPrintF("ZZ9000 Zorro 3 Version detected.\n");
		}
		else if((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6D6E, 0x3))) {
			// ZORRO 2
			MhiLibBase->zorro_version = 2;
			KPrintF("ZZ9000 Zorro 2 Version detected.\n");
		}
	} else {
		KPrintF("Error: Can't open expansion.library.\n");
		return FALSE;		
	}
	CloseLibrary((struct Library*)ExpansionBase);

	if(MhiLibBase->zorro_version == 0) {
		KPrintF("Error: ZZ9000 not detected.\n");
		return FALSE;		
	}

	hw_addr = (ULONG)cd->cd_BoardAddr;
	ax_present = *((volatile UWORD*)(hw_addr+REG_ZZ_AUDIO_CONFIG));
	if(!ax_present) {
		KPrintF("Error: ZZ9000AX not detected.\n");
		return FALSE;
	}

	KPrintF("HwAddr=0x%08lX\n", hw_addr);
	MhiLibBase->hw_addr = hw_addr;

	MhiLibBase->flags = 0;
	BPTR fh;
	if((fh=Open((CONST_STRPTR)"ENV:ZZ9K_INT2",MODE_OLDFILE))) {
		Close(fh);
		MhiLibBase->flags |= DEVF_INT2MODE;
	}

	MhiLibBase->NumAllocatedDecoders = 0;
	return TRUE;
}

void UserLibCleanup(struct MHI_LibBase *MhiLibBase) {
	// Nothing to clean up here because UserLibInit() didn't leave anything open.
}

ULONG cdev_isr(struct MhiPlayer *mp asm("a1")) {
	UWORD status = *(UWORD*)(mp->hw_addr+REG_ZZ_CONFIG);

	// audio interrupt signal set?
	if(status & 2) {
		// ack/clear audio interrupt
		*(USHORT*)(mp->hw_addr+REG_ZZ_CONFIG) = 8|32;

		if(mp->worker_process) {
			Signal((struct Task*)mp->worker_process, 1L << mp->enable_signal);
		}
	}
	return 0;
}

extern ULONG dev_isr(struct MhiPlayer *mp asm("a1"));

void init_interrupt(struct MhiPlayer *mp) {
	mp->irq.is_Node.ln_Type = NT_INTERRUPT;
	mp->irq.is_Node.ln_Pri = -60;
	mp->irq.is_Node.ln_Name = "mhizz9000";
	mp->irq.is_Data = mp;
	mp->irq.is_Code = (void*)dev_isr;

	Forbid();
	if (mp->flags & DEVF_INT2MODE) {
		AddIntServer(INTB_PORTS, &mp->irq);
	} else {
		AddIntServer(INTB_EXTER, &mp->irq);
	}
	Permit();

	// enable HW interrupt
	*(volatile USHORT*)(mp->hw_addr + REG_ZZ_AUDIO_CONFIG) = 1;
}

void destroy_interrupt(struct MhiPlayer *mp) {
	// disable HW interrupt
	*(volatile USHORT*)(mp->hw_addr + REG_ZZ_AUDIO_CONFIG) = 0;

	Forbid();
	if (mp->flags & DEVF_INT2MODE) {
		RemIntServer(INTB_PORTS, &mp->irq);
	} else {
		RemIntServer(INTB_EXTER, &mp->irq);
	}
	Permit();
}

static void WorkerProcess(void) {
	struct Process *proc = (struct Process *) FindTask(NULL);
	struct MhiPlayer *mp = proc->pr_Task.tc_UserData;

	mp->worker_signal = AllocSignal(-1);
	mp->enable_signal = AllocSignal(-1);

	ULONG signals = 0;
	ULONG buf_offset = 0;
	ULONG buf_samples = ZZ_BYTES_PER_PERIOD/4;

	Signal(mp->t_mainproc, 1L << mp->mainproc_signal);

	for(;;) {
  		signals = Wait(SIGBREAKF_CTRL_C | (1L << mp->enable_signal));
  		if(signals & SIGBREAKF_CTRL_C) break;

		fillFifo(mp);
		
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_SCALE)) = buf_samples;
		
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 4;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = (mp->decode_offset+buf_offset)>>16;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 5;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = (mp->decode_offset+buf_offset)&0xffff;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
		// ZZ_DECODE (task)
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODE)) = DECODE_RUN;
		
		// play buffer
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_SWAB)) = (1<<15) | (buf_offset >> 8); // no byteswap, offset/256
		int overrun = *((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_SWAB));
		
		if (overrun == 1) {
		  buf_offset = 0;
		} else {
		  buf_offset += ZZ_BYTES_PER_PERIOD;
		}
		
		if (buf_offset >= AUDIO_BUFSZ) {
		  buf_offset = 0;
		}

	}

	if(mp->enable_signal) FreeSignal(mp->enable_signal);
	mp->enable_signal = -1;
	if(mp->worker_signal) FreeSignal(mp->worker_signal);
	mp->worker_signal = -1;
	Signal((struct Task *)mp->t_mainproc, 1L << mp->mainproc_signal);
}

/*
 *
 */
APTR i_MHIAllocDecoder(REGA0(struct Task *mhi_task), REGD0(ULONG mhi_sigmask), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = NULL;

	// We only support one exclusive decoder allocation.
	if(MHI_LibBase->NumAllocatedDecoders) {
		return NULL;
	}

	mp = AllocVec(sizeof(struct MhiPlayer), MEMF_CLEAR);
	if(mp) {
		mp->hw_addr = MHI_LibBase->hw_addr;

		if (MHI_LibBase->zorro_version == 3) {
			// FIFO offset as in axmp3 (this is still within Zorro3 address range).
			mp->encoded_offset =  0x06000000;
			// Decoded audio offset right after FIFO with a little padding to be cache line aligned.
			mp->decode_offset  = (0x06000000 + FIFOSIZE + 32) & 0xFFFFFFE0;
		} else {
			// FIFO offset as in axmp3 (this is still within Zorro2 address range).
			// This probably needs to be adjusted to be guaranteed to be out of framebuffer memory.
			mp->encoded_offset = 0x100000;
			// Decoded audio offset at 96MB.
			mp->decode_offset  = 0x06000000;
		}
		KPrintF("encoded_offset = 0x%08lX\n", mp->encoded_offset);
		KPrintF("decode_offset  = 0x%08lX\n", mp->decode_offset);

		mp->mp3_addr = MHI_LibBase->hw_addr + 0x10000 + mp->encoded_offset;
		
		mp->decode_chunk_sz = 1920; // 16 bit sample pairs

		mp->MhiTask      = mhi_task;
		mp->MhiMask      = mhi_sigmask;
		mp->Status       = MHIF_STOPPED;

		mp->FifoMode     = FIFO_PREFILL;
		mp->FifoWriteIdx = 0;

		mp->BufferList = AllocVec(sizeof(struct MinList), MEMF_PUBLIC|MEMF_CLEAR);
		if(mp->BufferList) {
			NewList((struct List *)mp->BufferList);
			MHI_LibBase->NumAllocatedDecoders++;
			return mp;
		}
		FreeVec(mp);
	}
	return NULL;
}


/*
 *
 */
void i_MHIFreeDecoder(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	if(mp) {
		// reset mixer volume
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 10;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL))	 = 128 | 64<<8;
		*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;	

		if(mp->BufferList) {
			APTR killednode;
			while(killednode = RemHead((struct List *)mp->BufferList)) {
				FreeVec(killednode);
			}
			FreeVec(mp->BufferList);
		}
		FreeVec(mp);
	}
	if(MHI_LibBase->NumAllocatedDecoders) MHI_LibBase->NumAllocatedDecoders--;
}


/*
 *
 */
BOOL i_MHIQueueBuffer(REGA3(APTR mhi_handle), REGA0(APTR mhi_buffer), REGD0(ULONG mhi_size), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;
	if(mp != NULL) {
		
		BufferNode = AllocVec(sizeof(struct ListNode), MEMF_PUBLIC|MEMF_CLEAR);
		BufferNode->Buffer = mhi_buffer;
		BufferNode->Size   = mhi_size;
		BufferNode->Index  = 0;
		BufferNode->Played = FALSE;
		AddTail((struct List *)mp->BufferList, (struct Node *)BufferNode);

		KPrintF("MHIQueueBuffer: Adr=0x%08lX\n", mhi_buffer);

		return TRUE;
	}

	return FALSE;
}


/*
 *
 */
APTR i_MHIGetEmpty(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	struct ListNode *BufferNode;
	APTR mhi_buffer = NULL;

	/* Fetch first free buffer and return its memory pointer to the caller */
	if(mp) {
		for(;;) {
			BufferNode = (struct ListNode *)mp->BufferList->mlh_Head;
			if(BufferNode == NULL) break;
			if(BufferNode->Header.mln_Succ == NULL) break;
			if(BufferNode->Played == FALSE) break;

			mhi_buffer = BufferNode->Buffer;
			RemHead((struct List *)mp->BufferList);
			FreeVec(BufferNode);
		}
	}
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

	if(mp) {
		switch(mp->Status) {
			case MHIF_STOPPED:
				KPrintF("MHIPlay: Clearing FIFO.\n");
				clearFifo(mp);
				KPrintF("MHIPlay: Fillng FIFO.\n");
				fillFifo(mp);
				
				// set tx buffer address to 127 MB offset
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL)) = mp->decode_offset>>16;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 1;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL)) = mp->decode_offset&0xffff;
				
				// set LPF to 20KHz
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 9;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL)) = 20000;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;
				
				// set decoder params
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->encoded_offset>>16;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 1;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->encoded_offset&0xffff;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 2;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = FIFOSIZE>>16;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 3;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = FIFOSIZE&0xffff;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 4;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->decode_offset>>16;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 5;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->decode_offset&0xffff;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 6;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->decode_chunk_sz>>16;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 7;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_VAL)) = mp->decode_chunk_sz&0xffff;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
				
				// ZZ_DECODE (init)
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_DECODE)) = DECODE_INIT;

				mp->t_mainproc = FindTask(NULL);
				mp->mainproc_signal = AllocSignal(-1);
	
	
				if(mp->mainproc_signal != -1) {
					KPrintF("MHIPlay: mainproc signal allocd.\n");
					Forbid();
					if(mp->worker_process = CreateNewProcTags(NP_Entry, (ULONG)&WorkerProcess,
															  NP_Name, (ULONG)"mhizz9kwork",
															  NP_Priority, WORKER_PRIORITY,
															  TAG_DONE)) {
						mp->worker_process->pr_Task.tc_UserData = mp;
					}
					Permit();
				
					if(mp->worker_process) {
						KPrintF("MHIPlay: worker process created.\n");
						Wait(1L << mp->mainproc_signal);
						KPrintF("MHIPlay: worker process responded.\n");
						init_interrupt(mp);
						KPrintF("MHIPlay: interrupt inited.\n");
					}
				}
			
				mp->Status = MHIF_PLAYING;
			break;
			case MHIF_PAUSED:			
				// enable HW interrupt
				*(volatile USHORT*)(mp->hw_addr + REG_ZZ_AUDIO_CONFIG) = 1;
				mp->Status = MHIF_PLAYING;
			break;
		}
	}
}


/*
 *
 */
void i_MHIStop(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;
	APTR killednode;

	KPrintF("MHIStop called\n");

	if(mp) {
		switch(mp->Status) {
			case MHIF_PLAYING:
			case MHIF_PAUSED:
			case MHIF_OUT_OF_DATA:
				Signal((struct Task *)mp->worker_process, SIGBREAKF_CTRL_C);
				Wait(1L << mp->mainproc_signal);
				destroy_interrupt(mp);
				while(killednode = RemHead((struct List *)mp->BufferList)) {
					FreeVec(killednode);
				}
				mp->Status = MHIF_STOPPED;
			break;
		}
	}
}


/*
 *
 */
void i_MHIPause(REGA3(APTR mhi_handle), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	KPrintF("MHIPause called\n");

	if(mp) {
		switch(mp->Status) {
			case MHIF_PLAYING:
				// disable HW interrupt
				*(volatile USHORT*)(mp->hw_addr + REG_ZZ_AUDIO_CONFIG) = 0;
				mp->Status = MHIF_PAUSED;
			break;
		}
	}
}


/*
 *
 */
ULONG i_MHIQuery(REGD1( ULONG mhi_query), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	KPrintF("MHIQuery: query = %ld\n", mhi_query);

	switch(mhi_query) {
		case 0: // Inofficial capability string.
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

//		case MHIQ_BASS_CONTROL:
//		case MHIQ_TREBLE_CONTROL:
//			return MHIF_SUPPORTED;


		default:
			return MHIF_UNSUPPORTED;
	}
}

/*
 *
 */
void i_MHISetParam(REGA3(APTR mhi_handle), REGD0(UWORD mhi_param), REGD1( ULONG mhi_value), REGA6(struct MHI_LibBase *MHI_LibBase)) {
	struct MhiPlayer *mp = (struct MhiPlayer *)mhi_handle;

	if(mp) {
		switch(mhi_param) {
			case MHIP_PANNING: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				break;

			case MHIP_VOLUME: // 0..100
				if(mhi_value > 100) mhi_value = 100;
				// set mixer volume
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 10;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_VAL))	 = 128 | (mhi_value*64/100)<<8;
				*((volatile UWORD*)(mp->hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;	
				break;

			case MHIP_PREFACTOR: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				break;

			case MHIP_BASS: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				break;

			case MHIP_TREBLE: // 0..50..100
				if(mhi_value > 100) mhi_value = 100;
				break;

			default:
				KPrintF("MHISetParam: Unknown parameter %ld, value = %ld\n", mhi_param, mhi_value);
				break;
		}
	}
}

