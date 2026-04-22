/*
 * MNT ZZ9000 Network Driver (ZZ9000Net.device)
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * Based on code copyright (C) 2018 Henryk Richter <henryk.richter@gmx.net>
 * Released under GPLv3+ with permission.
 *
 * 2026 GCC port, bug fixes and performance work:
 *   Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#define DEVICE_MAIN

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/timer.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <exec/lists.h>
#include <exec/errors.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <string.h>

#ifdef HAVE_VERSION_H
#include "version.h"
#endif
/* NSD support is optional */
#ifdef NEWSTYLE
#include <devices/newstyle.h>
#endif /* NEWSTYLE */
#ifdef DEVICES_NEWSTYLE_H

const UWORD dev_supportedcmds[] = {
	NSCMD_DEVICEQUERY,
	CMD_READ,
	CMD_WRITE,
	/* ... add all cmds here that are supported by BeginIO */
	0
};

const struct NSDeviceQueryResult NSDQueryAnswer = {
	0,
	16, /* up to SupportedCommands (inclusive) TODO: correct number */
	NSDEVTYPE_SANA2, /* TODO: proper device type */
	0,  /* subtype */
	(UWORD*)dev_supportedcmds
};
#endif /* DEVICES_NEWSTYLE_H */

#include "device.h"
#include "macros.h"

// FIXME get rid of global var!
static ULONG ZZ9K_REGS = 0;
#define ZZ9K_RX 0x2000
#define ZZ9K_TX 0x8000

struct Sana2DeviceStats global_stats;
BOOL is_online;

SAVEDS void frame_proc();
char *frame_proc_name = "ZZ9000NetFramer";

/* ZZ9000 interrupt server (INT6 default, optional INT2).
 * Reads the status once, masks+acks the ethernet bit, signals frame_proc.
 * Returns non-zero when this interrupt was ours so Exec short-circuits
 * the server chain; chains through (returns 0) when it isn't.
 */
SAVEDS ULONG dev_isr(struct devbase* db __asm("a1")) {
  volatile USHORT* status_reg = (volatile USHORT*)(ZZ9K_REGS+0x04);
  USHORT status = *status_reg;

  if (!(status & 1)) {
    return 0;
  }

  /* Disable the ethernet IRQ bit, then ack it. frame_proc re-enables bit 0. */
  *status_reg = status & 0xfffe;
  *status_reg = 8|16;

  if (db->db_Proc) {
    Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_F);
  }
  return 1;
}

static UBYTE HW_MAC[] = {0x00,0x00,0x00,0x00,0x00,0x00};

void set_mac_from_string(UBYTE* buf) {
  int k=0;
  for (int i=0; i<6; i++) {
    int c = buf[k];
    int v = 0;

    if (c>='0' && c<='9') c-='0';
    else if (c>='a' && c<='f') c=c+10-'a';
    else if (c>='A' && c<='F') c=c+10-'A';

    v = c<<4;
    c = buf[k+1];

    if (c>='0' && c<='9') c-='0';
    else if (c>='a' && c<='f') c=c+10-'a';
    else if (c>='A' && c<='F') c=c+10-'A';

    HW_MAC[i] = v+c;

    k+=3;
  }
}

struct ProcInit
{
   struct Message msg;
   struct devbase *db;
   BOOL  error;
   UBYTE pad[2];
};

SAVEDS struct Device *DevInit( ASMR(d0) DEVBASEP                  ASMREG(d0),
                                   ASMR(a0) BPTR seglist              ASMREG(a0),
				   ASMR(a6) struct Library *_SysBase  ASMREG(a6) )
{
	UBYTE*p;
	ULONG i;
	LONG  ok;

	p = ((UBYTE*)db) + sizeof(struct Library);
	i = sizeof(DEVBASETYPE)-sizeof(struct Library);
	while( i-- )
		*p++ = 0;

	db->db_SysBase = _SysBase;
	db->db_SegList = seglist;
	db->db_Flags   = 0;

	ok = 0;
	if( (DOSBase = OpenLibrary("dos.library", 36)) ) {
		if( (UtilityBase = OpenLibrary("utility.library", 37)) ) {
			ok = 0;

      struct ConfigDev* cd = NULL;

      if ((ExpansionBase = OpenLibrary("expansion.library", 0)) ) {
        // Find Z2 or Z3 model of MNT ZZ9000
        if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4)) || (cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) {
          BPTR fh;

          D(("ZZ9000Net: MNT ZZ9000 found.\n"));
          ZZ9K_REGS = (ULONG)cd->cd_BoardAddr;

          // Thanks to https://grandcentrix.team
          HW_MAC[0]=0x68;
          HW_MAC[1]=0x82;
          HW_MAC[2]=0xF2;
          HW_MAC[3]=0x00;
          HW_MAC[4]=0x01;
          HW_MAC[5]=0x00;

          if ((fh=Open("ENV:ZZ9K_MAC",MODE_OLDFILE))) {
            UBYTE char_buf[32];
            char* res = FGets(fh,char_buf,18);
            if (!res || strlen(char_buf)<17) {
              D(("ZZ9000Net: MAC address in ENV:ZZ9K_MAC has invalid syntax.\n"));
            } else {
              D(("ZZ9000Net: Setting MAC address from ENV:ZZ9K_MAC.\n"));
              set_mac_from_string(char_buf);
            }
            Close(fh);
          }

          // FIXME
          *(volatile USHORT*)(ZZ9K_REGS+0x84) = (HW_MAC[0]<<8)|HW_MAC[1];
          *(volatile USHORT*)(ZZ9K_REGS+0x84) = (HW_MAC[0]<<8)|HW_MAC[1];
          *(volatile USHORT*)(ZZ9K_REGS+0x86) = (HW_MAC[2]<<8)|HW_MAC[3];
          *(volatile USHORT*)(ZZ9K_REGS+0x86) = (HW_MAC[2]<<8)|HW_MAC[3];
          *(volatile USHORT*)(ZZ9K_REGS+0x88) = (HW_MAC[4]<<8)|HW_MAC[5];

          ok = 1;

        } else {
          D(("ZZ9000Net: MNT ZZ9000 not found!\n"));
        }
				CloseLibrary(ExpansionBase);
      } else {
        D(("ZZ9000Net: failed to open expansion.library!\n"));
      }

			if (!ok) {
				CloseLibrary(DOSBase);
				CloseLibrary(UtilityBase);
			}
		}
		else {
			D(("ZZ9000Net: Could not open utility.library.\n"));
			CloseLibrary(DOSBase);
		}
	}
	else {
		D(("ZZ9000Net: Could not open dos.library.\n"));
	}

	{
		BPTR fh;
		if ((fh=Open("ENV:ZZ9K_INT2",MODE_OLDFILE))) {
			D(("ZZ9000Net: Using INT2 mode.\n"));
			Close(fh);
			db->db_Flags |= DEVF_INT2MODE;
		} else {
			D(("ZZ9000Net: Using INT6 mode (default).\n"));
		}
	}

	/* no hardware found, reject init */
	return (ok > 0) ? (struct Device*)db : (0);
}

SAVEDS LONG DevOpen( ASMR(a1) struct IOSana2Req *ioreq           ASMREG(a1),
                         ASMR(d0) ULONG unit                         ASMREG(d0),
                         ASMR(d1) ULONG flags                        ASMREG(d1),
                         ASMR(a6) DEVBASEP                           ASMREG(a6) )
{
	LONG ok = 0,ret = IOERR_OPENFAIL;
  struct BufferManagement *bm;

	D(("ZZ9000Net: DevOpen for %ld\n",unit));

	db->db_Lib.lib_OpenCnt++; /* avoid Expunge, see below for separate "unit" open count */

  if (unit==0 && db->db_Lib.lib_OpenCnt==1) {
    if ((bm = (struct BufferManagement*)AllocVec(sizeof(struct BufferManagement), MEMF_CLEAR|MEMF_PUBLIC))) {
      bm->bm_CopyToBuffer = (BMFunc)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
      bm->bm_CopyFromBuffer = (BMFunc)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);

      ioreq->ios2_BufferManagement = (VOID *)bm;
      ioreq->ios2_Req.io_Error = 0;
      ioreq->ios2_Req.io_Unit = (struct Unit *)unit; // not a real pointer, but id integer
      ioreq->ios2_Req.io_Device = (struct Device *)db;

      memset(&global_stats, 0, sizeof(global_stats));

      NEWLIST(&db->db_ReadList);
      InitSemaphore(&db->db_ReadListSem);

      struct ProcInit init;
      struct MsgPort *port;

      if (port = CreateMsgPort()) {
        D(("ZZ9000Net: Starting Process\n"));
        if ((db->db_Proc = CreateNewProcTags(NP_Entry, (ULONG)frame_proc, NP_Name,
                                             (ULONG)frame_proc_name, NP_Priority, 0, TAG_DONE))) {
          InitSemaphore(&db->db_ProcExitSem);

          init.error = 1;
          init.db = db;
          init.msg.mn_Length = sizeof(init);
          init.msg.mn_ReplyPort = port;

          D(("ZZ9000Net: handover db: %lx\n",init.db));

          PutMsg(&db->db_Proc->pr_MsgPort, (struct Message*)&init);
          WaitPort(port);

          if (!init.error) {
            ok = 1;

            // Register Interrupt server
            if ((db->db_interrupt = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC|MEMF_CLEAR))) {
              db->db_interrupt->is_Node.ln_Type = NT_INTERRUPT;
              db->db_interrupt->is_Node.ln_Pri = 125;
              db->db_interrupt->is_Node.ln_Name = "ZZ9000Net";
              db->db_interrupt->is_Data = (APTR)db;
              db->db_interrupt->is_Code = (void(*)())dev_isr;

              Disable();
              AddIntServer((db->db_Flags & DEVF_INT2MODE) ? INTB_PORTS : INTB_EXTER, db->db_interrupt);
              Enable();

              D(("ZZ9000Net: Interrupt server registered, using INT%ld\n",(db->db_Flags & DEVF_INT2MODE) ? 2L : 6L));
              ret = 0;
              ok = 1;

              // enable HW interrupt
              *(volatile USHORT*)(ZZ9K_REGS+0x04) = 1;

              D(("ZZ9000Net: ZZ interrupt enabled\n"));
            } else {
              D(("ZZ9000Net: failed to alloc struct Interrupt\n"));
              ret = IOERR_OPENFAIL;
              ok = 0;

              Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_C);
              // this will block until the process has really quit and released the semaphore
              ObtainSemaphore(&db->db_ProcExitSem);
              ReleaseSemaphore(&db->db_ProcExitSem);
            }
          } else {
            D(("ZZ9000Net:process startup error\n"));
            ret = IOERR_OPENFAIL;
            ok = 0;
          }
        } else {
          D(("ZZ9000Net:couldn't create process\n"));
          ret = IOERR_OPENFAIL;
          ok = 0;
        }
        DeleteMsgPort(port);
      }
    }
  } else {
    ret = IOERR_OPENFAIL;
    ok = 0;
  }

	if (ok) {
		ret = 0;
    db->db_Lib.lib_Flags &= ~LIBF_DELEXP;
	}

	if (ret == IOERR_OPENFAIL) {
		ioreq->ios2_Req.io_Unit   = (0);
		ioreq->ios2_Req.io_Device = (0);
		ioreq->ios2_Req.io_Error  = ret;
		db->db_Lib.lib_OpenCnt--;
	}
	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;

	D(("ZZ9000Net: DevOpen return code %ld\n",ret));

	return ret;
}

SAVEDS BPTR DevClose(   ASMR(a1) struct IORequest *ioreq        ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	/* ULONG unit; */
	BPTR  ret = (0);

	D(("ZZ9000Net: DevClose open count %ld\n",db->db_Lib.lib_OpenCnt));

	if (!ioreq)
		return ret;

  // disable HW interrupt
  *(volatile USHORT*)(ZZ9K_REGS+0x04) = 0;
  D(("ZZ9000Net: ZZ interrupt disabled\n"));

	db->db_Lib.lib_OpenCnt--;

  if (db->db_Lib.lib_OpenCnt == 0) {
    if (db->db_interrupt) {
      D(("ZZ9000Net: Remove IntServer...\n"));
      Forbid();
      RemIntServer((db->db_Flags & DEVF_INT2MODE) ? INTB_PORTS : INTB_EXTER, db->db_interrupt);
      db->db_interrupt = 0;
      Permit();
    }
    if (db->db_Proc) {
      D(("ZZ9000Net: End Proc...\n"));
      Signal((struct Task*)db->db_Proc, SIGBREAKF_CTRL_C);
      db->db_Proc = 0;

      ObtainSemaphore(&db->db_ProcExitSem);
      ReleaseSemaphore(&db->db_ProcExitSem);
    }
  }

	ioreq->io_Device = (0);
	ioreq->io_Unit   = (struct Unit *)(-1);

	if (db->db_Lib.lib_Flags & LIBF_DELEXP)
		ret = DevExpunge(db);

	return ret;
}

SAVEDS BPTR DevExpunge( ASMR(a6) DEVBASEP                        ASMREG(a6) )
{
	BPTR seglist = db->db_SegList;

	if( db->db_Lib.lib_OpenCnt )
	{
		db->db_Lib.lib_Flags |= LIBF_DELEXP;
		return (0);
	}

  D(("ZZ9000Net: Remove Device Node...\n"));
  Remove((struct Node*)db);

	CloseLibrary(DOSBase);
	CloseLibrary(UtilityBase);
	FreeMem( ((BYTE*)db)-db->db_Lib.lib_NegSize,(ULONG)(db->db_Lib.lib_PosSize + db->db_Lib.lib_NegSize));

	return seglist;
}

struct Device *TimerBase;
static void set_last_start()
{
  struct { void *db_SysBase; } *db = (void*)0x4;
  struct IORequest req;
  memset(&req, 0, sizeof(req));
  req.io_Message.mn_Length = sizeof(req);

  if (OpenDevice(TIMERNAME, UNIT_MICROHZ, &req, 0) == 0)
  {
    TimerBase = req.io_Device;
    GetSysTime(&global_stats.LastStart);
    CloseDevice(&req);
  }
}

ULONG read_frame(struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp);
ULONG write_frame(struct IOSana2Req *req, UBYTE *frame);

SAVEDS VOID DevBeginIO( ASMR(a1) struct IOSana2Req *ioreq       ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	ULONG unit = (ULONG)ioreq->ios2_Req.io_Unit;
	(void)unit;

	ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
  ioreq->ios2_Req.io_Error = S2ERR_NO_ERROR;
  ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;

	//D(("BeginIO command %ld unit %ld\n",(LONG)ioreq->ios2_Req.io_Command,unit));

	switch( ioreq->ios2_Req.io_Command ) {
  case CMD_READ:
    if (ioreq->ios2_BufferManagement == NULL) {
      ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
      ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
    }
    else {
      // not quick, add request to reader list
      // will be handled on interrupts by frame_proc
      ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
      ObtainSemaphore(&db->db_ReadListSem);
      AddHead((struct List*)&db->db_ReadList, (struct Node*)ioreq);
      ReleaseSemaphore(&db->db_ReadListSem);
      ioreq = NULL;
    }
    break;
  case S2_BROADCAST:
    /* set broadcast addr: ff:ff:ff:ff:ff:ff */
    if (ioreq->ios2_DstAddr) {
      memset(ioreq->ios2_DstAddr, 0xff, HW_ADDRFIELDSIZE);
    } else {
      D(("bcast: invalid dst addr\n"));
    }
    /* fall through */
  case CMD_WRITE: {
    ULONG res = write_frame(ioreq, (UBYTE*)(ZZ9K_REGS+ZZ9K_TX));
    if (res!=0) {
      ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
      ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;
    } else {
      ioreq->ios2_Req.io_Error = 0;
      global_stats.PacketsSent++;
    }
    break;
  }

  case S2_READORPHAN:
    if (!ioreq->ios2_BufferManagement)
			{
				ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
				ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
			}
    else
			{
        ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
        // FIXME do we need this list?
        //ObtainSemaphore(&db->db_Units[unit].du_Sem);
        //AddHead((struct List*)&db->db_ReadOrphans,(struct Node*)ioreq);
        //ReleaseSemaphore(&db->db_Units[unit].du_Sem);
        ioreq = NULL;
			}
    break;

  case S2_CONFIGINTERFACE:   /* forward request */
    /* fall through */
  case S2_ONLINE:
    set_last_start();
    is_online = TRUE;
    break;
  case S2_OFFLINE:
    is_online = FALSE;
    break;

  case S2_GETSTATIONADDRESS:
    memcpy(ioreq->ios2_SrcAddr, HW_MAC, HW_ADDRFIELDSIZE); /* current */
    memcpy(ioreq->ios2_DstAddr, HW_MAC, HW_ADDRFIELDSIZE); /* default */
    break;
  case S2_DEVICEQUERY:
    {
      struct Sana2DeviceQuery *devquery;

      devquery = ioreq->ios2_StatData;
      devquery->DevQueryFormat = 0;        /* "this is format 0" */
      devquery->DeviceLevel = 0;           /* "this spec defines level 0" */

      if (devquery->SizeAvailable >= 18) devquery->AddrFieldSize = HW_ADDRFIELDSIZE * 8; /* Bits! */
      if (devquery->SizeAvailable >= 22) devquery->MTU           = 1500;
      if (devquery->SizeAvailable >= 26) devquery->BPS           = 1000*1000*100;
      if (devquery->SizeAvailable >= 30) devquery->HardwareType  = S2WireType_Ethernet;

      devquery->SizeSupplied = (devquery->SizeAvailable<30?devquery->SizeAvailable:30);
    }
    break;
  case S2_GETGLOBALSTATS:
    if (ioreq->ios2_StatData) {
      memcpy(ioreq->ios2_StatData, &global_stats, sizeof(struct Sana2DeviceStats));
    }
    break;
  case S2_GETSPECIALSTATS:
    {
      struct Sana2SpecialStatHeader *s2ssh = (struct Sana2SpecialStatHeader *)ioreq->ios2_StatData;
      if (s2ssh) s2ssh->RecordCountSupplied = 0;
    }
    break;
  default:
    {
      ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
      ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;
      break;
    }
	}

	if (ioreq) {
		DevTermIO(db, (struct IORequest*)ioreq);
  }
}

SAVEDS LONG DevAbortIO( ASMR(a1) struct IORequest *ioreq        ASMREG(a1),
                            ASMR(a6) DEVBASEP                       ASMREG(a6) )
{
	struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;
	struct Node* n;
	LONG ret = -1;

	D(("ZZ9000Net: AbortIO on %lx\n",(ULONG)ioreq));

	/* Walk the read list under the semaphore to make sure the IO is still
	 * pending (and not already being serviced by frame_proc). Only then is
	 * it safe to Remove()/Reply it; otherwise the caller gets -1 meaning
	 * "IO was not abortable". */
	ObtainSemaphore(&db->db_ReadListSem);
	for (n = db->db_ReadList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
		if (n == (struct Node*)ioreq) {
			Remove(n);
			ret = 0;
			break;
		}
	}
	ReleaseSemaphore(&db->db_ReadListSem);

	if (ret == 0) {
		ioreq->io_Error = IOERR_ABORTED;
		ios2->ios2_WireError = 0;
		ReplyMsg((struct Message*)ioreq);
	}
	return ret;
}

void DevTermIO( DEVBASEP, struct IORequest *ioreq )
{
  struct IOSana2Req* ios2 = (struct IOSana2Req*)ioreq;

  if (!(ios2->ios2_Req.io_Flags & SANA2IOF_QUICK)) {
    ReplyMsg((struct Message *)ioreq);
  } else {
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
  }
}

/* Frame header layout in the ZZ9000 RX window (MMIO-backed):
 *   +0..+1   USHORT  total size
 *   +2..+3   USHORT  serial (increments each new frame)
 *   +4..+9   UBYTE[] destination MAC
 *   +10..+15 UBYTE[] source MAC
 *   +16..+17 USHORT  ethertype
 *   +18..    payload
 *
 * Byte-wise shift-and-OR loads used to cost two MMIO cycles each. Word
 * reads are a single bus cycle on a word-aligned address, which roughly
 * halves the per-packet overhead on Zorro. */

static inline USHORT zznet_read_word(volatile UBYTE *frame, ULONG offset) {
	return *(volatile USHORT*)(frame + offset);
}

/* Fetch [size:2][serial:2] in one bus cycle on Z3 (32-bit) — the two
 * values always move together and live in adjacent words, so there is
 * no reason to poke the card twice. Caller gets them back via the out
 * params. */
static inline void zznet_read_header(volatile UBYTE *frame, USHORT *size, USHORT *serial) {
	ULONG hdr = *(volatile ULONG*)frame;
	*size   = (USHORT)(hdr >> 16);
	*serial = (USHORT)(hdr & 0xFFFF);
}

ULONG read_frame(struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp)
{
	struct BufferManagement *bm;
	volatile UBYTE *frame_ptr;
	ULONG datasize;
	ULONG err = 0;

	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		frame_ptr = frame + 4;
		datasize  = sz;
		req->ios2_Req.io_Flags = SANA2IOF_RAW;
	} else {
		frame_ptr = frame + 4 + HW_ETH_HDR_SIZE;
		datasize  = (ULONG)sz - HW_ETH_HDR_SIZE;
		req->ios2_Req.io_Flags = 0;
	}

	req->ios2_DataLength = datasize;

	bm = (struct BufferManagement *)req->ios2_BufferManagement;
	if (!(*bm->bm_CopyToBuffer)((void*)req->ios2_Data, (void*)frame_ptr, datasize)) {
		req->ios2_Req.io_Error = S2ERR_SOFTWARE;
		req->ios2_WireError    = S2WERR_BUFF_ERROR;
		err = 1;
	} else {
		req->ios2_Req.io_Error = req->ios2_WireError = 0;
	}

	/* Coalesce the 12-byte dst+src MAC header into three longword MMIO reads
	 * instead of six word reads (and a second pass of three for the broadcast
	 * check). On Z3 that's 3 bus cycles instead of 9. The destination
	 * ios2_DstAddr / ios2_SrcAddr fields are word-aligned per SANA-II, so
	 * splitting each long back into two word stores is safe. */
	{
		ULONG m0 = *(volatile ULONG*)(frame + 4);    /* dst[0..3]           */
		ULONG m1 = *(volatile ULONG*)(frame + 8);    /* dst[4..5] src[0..1] */
		ULONG m2 = *(volatile ULONG*)(frame + 12);   /* src[2..5]           */

		USHORT *wd = (USHORT*)req->ios2_DstAddr;
		USHORT *ws = (USHORT*)req->ios2_SrcAddr;
		wd[0] = (USHORT)(m0 >> 16);
		wd[1] = (USHORT)(m0 & 0xFFFF);
		wd[2] = (USHORT)(m1 >> 16);
		ws[0] = (USHORT)(m1 & 0xFFFF);
		ws[1] = (USHORT)(m2 >> 16);
		ws[2] = (USHORT)(m2 & 0xFFFF);

		if (m0 == 0xFFFFFFFFUL && (m1 & 0xFFFF0000UL) == 0xFFFF0000UL) {
			req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
		}
	}

	req->ios2_PacketType = tp;

	return err;
}

ULONG write_frame(struct IOSana2Req *req, UBYTE *frame)
{
	struct BufferManagement *bm;
	USHORT sz = 0;
	ULONG  rc = 0;

	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		sz = req->ios2_DataLength;
	} else {
		sz = req->ios2_DataLength + HW_ETH_HDR_SIZE;

		/* Build the 14-byte Ethernet header. Using memcpy (non-volatile
		 * frame pointer) lets libc / the compiler emit move.l where the
		 * alignment permits; forcing word stores here costs Zorro III
		 * bandwidth versus the baseline. The reg write below is volatile
		 * and serves as the commit barrier before we kick TX. */
		*((USHORT*)(frame + 12)) = (USHORT)req->ios2_PacketType;
		memcpy(frame,     req->ios2_DstAddr, HW_ADDRFIELDSIZE);
		memcpy(frame + 6, HW_MAC,            HW_ADDRFIELDSIZE);
		frame += HW_ETH_HDR_SIZE;
	}

	if (sz == 0) {
		return 0;
	}

	bm = (struct BufferManagement *)req->ios2_BufferManagement;
	if (!(*bm->bm_CopyFromBuffer)(frame, req->ios2_Data, req->ios2_DataLength)) {
		return 1;
	}

	{
		volatile USHORT *reg = (volatile USHORT*)(ZZ9K_REGS+0x80);
		*reg = sz;      /* kick the TX engine */
		rc   = *reg;    /* read back hardware status */
		if (rc) {
			D(("tx err: %d\n",rc));
		}
	}

	return rc;
}

SAVEDS void frame_proc() {
  ULONG wmask;

  D(("ZZ9000Net: frame_proc()\n"));

  struct ProcInit* init;

  {
    struct { void *db_SysBase; } *db = (void*)0x4;
    struct Process* proc;

    proc = (struct Process*)FindTask(NULL);
    WaitPort(&proc->pr_MsgPort);
    init = (struct ProcInit*)GetMsg(&proc->pr_MsgPort);
  }

  struct devbase* db = init->db;

  init->error = 0;
  db = init->db;
  ObtainSemaphore(&db->db_ProcExitSem);
  ReplyMsg((struct Message*)init);

  wmask = SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C;

  USHORT old_serial = 0;
  ULONG  recv       = Wait(wmask);   /* wait for first packet */

  volatile UBYTE*  frm       = (volatile UBYTE*)(ZZ9K_REGS+ZZ9K_RX);
  volatile USHORT* rx_accept = (volatile USHORT*)(ZZ9K_REGS+0x82);
  volatile USHORT* irq_ctrl  = (volatile USHORT*)(ZZ9K_REGS+0x04);

  while (1) {
    struct IOSana2Req *ior;

    if (recv & SIGBREAKF_CTRL_C) {
      D(("ZZ9000Net: process end\n"));
      break;
    }

    USHORT sz, serial;
    zznet_read_header(frm, &sz, &serial);

    if (serial != old_serial) {
      USHORT packet_type = *(volatile USHORT*)(frm + 16);
      struct IOSana2Req *match = NULL;
      old_serial = serial;

      /* Walk the read list only long enough to find a matching listener
       * and detach it. Doing the payload copy (read_frame) and ReplyMsg
       * outside the semaphore keeps DevAbortIO / CMD_READ unblocked for
       * the duration of the Zorro bus copy. */
      ObtainSemaphore(&db->db_ReadListSem);
      for (ior = (struct IOSana2Req *)db->db_ReadList.lh_Head;
           ior->ios2_Req.io_Message.mn_Node.ln_Succ;
           ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
        if (ior->ios2_PacketType == packet_type) {
          Remove((struct Node*)ior);
          match = ior;
          break;
        }
      }
      ReleaseSemaphore(&db->db_ReadListSem);

      if (match) {
        ULONG res = read_frame(match, frm, sz, packet_type);
        if (res == 0) {
          global_stats.PacketsReceived++;
        } else {
          /* read_frame already set io_Error/ios2_WireError; reply so the
           * caller learns the request failed instead of leaving it on
           * a now-dangling list entry. */
          D(("RERR %ld\n", res));
          global_stats.UnknownTypesReceived++;
        }
        ReplyMsg((struct Message *)match);
      } else {
        /* No listener matched — frame dropped. A future change could
         * route these to S2_READORPHAN requests. */
        global_stats.UnknownTypesReceived++;
      }

      /* Release the FPGA RX slot so the next frame can land. We do NOT
       * re-enable the ethernet IRQ here — staying masked lets us drain
       * any already-queued frames via the serial recheck on the next
       * loop iteration without paying for an IRQ we'd ignore anyway. */
      *rx_accept = 1;
    } else {
      /* Nothing new. Re-enable the ethernet IRQ so the ISR can wake us,
       * then sleep. Enable-before-wait is correct: if a frame raced in
       * between our serial read and the enable, the ISR will signal
       * and Wait returns immediately. */
      *irq_ctrl = 1;
      recv = Wait(wmask);
    }
  }
  // disable interrupt
  *(volatile USHORT*)(ZZ9K_REGS+0x04) = 0;

  Forbid();
  ReleaseSemaphore(&db->db_ProcExitSem);
}
