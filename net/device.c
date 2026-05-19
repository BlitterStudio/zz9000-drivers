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

/* Staging buffer for RX payload copies.
 *
 * Roadshow's BufferManagement bm_CopyToBuffer is a generic memcpy. When
 * we hand it `frame + 18` (the non-RAW payload offset inside the FPGA
 * RX window), the source is only word-aligned (18 mod 4 == 2), so its
 * memcpy falls back to move.w at best. Every RX frame then burns
 * ~750 word-sized Zorro bus cycles instead of ~375 longword cycles.
 *
 * Staging the payload through a driver-owned RAM buffer lets us issue
 * the MMIO reads ourselves as longwords (one Z3 bus cycle per 4 bytes)
 * and then hand Roadshow a RAM source, where its generic memcpy is
 * essentially free relative to the MMIO read cost.
 *
 * 1536 covers any MTU up to 1514 plus a 2-byte alignment offset. The
 * buffer is single-use per frame and only touched from frame_proc
 * (single worker process per devbase, serialized by AmigaOS Forbid()
 * semantics on OpenDevice/CloseDevice), so no locking is needed. The
 * pointer lives on the devbase — see `struct devbase::db_RxStage`. */
#define RX_STAGE_SIZE 1536

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

  /* Multi-open support: the original driver rejected any opener past the
   * first. That blocks diagnostics like ZZNetStats from attaching while
   * Roadshow has the device open. SANA-II allows multiple openers with
   * independent BufferManagement — we do the heavy one-time init (RX
   * list, worker process, interrupt server, stats reset) only on the
   * very first open, and just hand the per-opener BM out on subsequent
   * opens. */
  if (unit==0 && db->db_Lib.lib_OpenCnt >= 1) {
    BOOL first_open = (db->db_Lib.lib_OpenCnt == 1);

    if ((bm = (struct BufferManagement*)AllocVec(sizeof(struct BufferManagement), MEMF_CLEAR|MEMF_PUBLIC))) {
      bm->bm_CopyToBuffer = (BMFunc)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
      bm->bm_CopyFromBuffer = (BMFunc)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);

      ioreq->ios2_BufferManagement = (VOID *)bm;
      ioreq->ios2_Req.io_Error = 0;
      ioreq->ios2_Req.io_Unit = (struct Unit *)unit; // not a real pointer, but id integer
      ioreq->ios2_Req.io_Device = (struct Device *)db;

      if (!first_open) {
        /* Secondary opener — hardware and worker process are already up.
         * Defensive: explicitly verify first-open init actually completed
         * (db_Proc and db_interrupt set) instead of trusting lib_OpenCnt
         * as a proxy. Under AmigaOS Forbid()-serialized OpenDevice this
         * race isn't reachable, but the explicit check is cheap and
         * protects against a future caller that opens without Forbid(). */
        if (db->db_Proc && db->db_interrupt) {
          ok  = 1;
          ret = 0;
        } else {
          D(("ZZ9000Net: secondary open rejected, first-open init incomplete\n"));
          FreeVec(bm);
          ioreq->ios2_BufferManagement = NULL;
          ret = IOERR_OPENFAIL;
          ok  = 0;
        }
      } else {

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

              /* Allocate the RX payload staging buffer in Fast RAM. This
               * runs only after all fallible first-open init has
               * succeeded, so a failure above cannot leak the buffer.
               * Failure here is non-fatal: read_frame falls back to a
               * direct bm_CopyToBuffer from MMIO when db_RxStage is
               * NULL. AllocVec is expected to return 8-byte alignment
               * per the Exec contract; the explicit runtime check keeps
               * us honest about zznet_mmio_read_block's longword
               * alignment precondition. */
              if (!db->db_RxStage) {
                db->db_RxStage = AllocVec(RX_STAGE_SIZE, MEMF_FAST);
                if (db->db_RxStage && ((ULONG)db->db_RxStage & 3)) {
                  D(("ZZ9000Net: AllocVec returned misaligned pointer %lx; refusing staging\n",
                     (ULONG)db->db_RxStage));
                  FreeVec(db->db_RxStage);
                  db->db_RxStage = NULL;
                }
                if (!db->db_RxStage) {
                  D(("ZZ9000Net: db_RxStage unavailable; falling back to direct MMIO copy\n"));
                }
              }

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
      } /* end first_open init */
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

	/* Free this opener's BufferManagement. Each OpenDevice allocates one;
	 * previously this was leaked on every close. */
	{
		struct IOSana2Req *s2 = (struct IOSana2Req *)ioreq;
		if (s2->ios2_BufferManagement) {
			FreeVec(s2->ios2_BufferManagement);
			s2->ios2_BufferManagement = NULL;
		}
	}

	db->db_Lib.lib_OpenCnt--;

  if (db->db_Lib.lib_OpenCnt == 0) {
    /* Last opener gone — disable HW IRQ, tear down worker process.
     * Previously the IRQ was disabled on every close, which killed
     * Roadshow's RX if a diagnostic tool opened+closed the device. */
    *(volatile USHORT*)(ZZ9K_REGS+0x04) = 0;
    D(("ZZ9000Net: ZZ interrupt disabled\n"));

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

    /* Staging buffer is freed only after the worker process has
     * exited above (ObtainSemaphore/ReleaseSemaphore pair). That
     * guarantees no read_frame can still be in flight referencing
     * db_RxStage, because all RX paths run inside frame_proc. */
    if (db->db_RxStage) {
      FreeVec(db->db_RxStage);
      db->db_RxStage = NULL;
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

ULONG read_frame(DEVBASETYPE *db, struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp);
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

/* Bulk MMIO→RAM copy for an RX payload.
 *
 * Source is the FPGA RX window (word-aligned for non-RAW at frame+18,
 * longword-aligned for RAW at frame+4). We place the destination
 * inside `base` at the same 2-byte phase as the source so the body of
 * the copy can be done in aligned longword reads on both sides — the
 * expensive side (MMIO) drops to 1 bus cycle per 4 bytes instead of 2.
 *
 * Returns the start of the valid bytes inside `base`, which the caller
 * then passes to Roadshow's CopyToBuffer as a RAM source.
 */
static inline UBYTE* zznet_mmio_read_block(volatile UBYTE *src, UBYTE *base, ULONG n) {
	/* Caller contract: `base` is longword-aligned (enforced at AllocVec
	 * time in DevOpen). `src` is word-aligned and its 2-byte phase
	 * picks whether the body of the copy starts at `base` (RAW, src
	 * 4-aligned) or `base + 2` (non-RAW, src at frame+18). After the
	 * leading-word branch fires (if any), both src and dst are
	 * longword-aligned, and they stay that way through the longword
	 * loop — so every typed wide access in this function lands on an
	 * aligned address. */
	UBYTE *dst = base + ((ULONG)src & 2);   /* match source 2-byte phase */
	UBYTE *dst_start = dst;

	if (n == 0) return dst_start;

	/* Consume a leading 2-byte misalignment once — but only when at
	 * least 2 bytes remain. A stray 1-byte payload (runt frame) would
	 * otherwise underflow the unsigned `n` and walk the longword loop
	 * across the whole FPGA window. For n == 1 we fall straight through
	 * to the final single-byte copy below. */
	if (((ULONG)src & 2) && n >= 2) {
		*(USHORT*)dst = *(volatile USHORT*)src;
		src += 2; dst += 2; n -= 2;
	}

	/* Bulk longword stream runs only when src is actually 4-aligned. If
	 * the leading-word branch was skipped because n < 2, src is still
	 * 2-aligned and we drop straight to the byte tail. */
	if (((ULONG)src & 3) == 0) {
		volatile ULONG *ls = (volatile ULONG*)src;
		ULONG          *ld = (ULONG*)dst;
		ULONG longs = n >> 2;
		while (longs--) *ld++ = *ls++;
		src = (volatile UBYTE*)ls;
		dst = (UBYTE*)ld;
		n &= 3;
	}

	if (n >= 2) {
		*(USHORT*)dst = *(volatile USHORT*)src;
		src += 2; dst += 2; n -= 2;
	}
	if (n) {
		*dst = *src;
	}

	return dst_start;
}

ULONG read_frame(DEVBASETYPE *db, struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp)
{
	struct BufferManagement *bm;
	volatile UBYTE *frame_ptr;
	ULONG datasize;
	ULONG err = 0;

	/* Size policy split by listener type:
	 *   - RAW   : accept up to HW_ETH_MAX_RAW  (1518, VLAN-tagged).
	 *   - non-RAW : accept up to HW_ETH_MAX_STD (1514, untagged), since
	 *               we advertise MTU = 1500 to the SANA-II client.
	 *
	 * frame_proc already drops wire-level garbage (sz < 18 or sz >
	 * HW_ETH_MAX_RAW) before matching a listener, so these checks only
	 * fire for frames that are protocol-shaped but don't fit the
	 * listener's contract — e.g. a VLAN-tagged frame delivered to a
	 * non-RAW listener. In that case returning an error is correct:
	 * the oversized frame genuinely cannot be delivered through the
	 * non-RAW path without overrunning the client's MTU-sized buffer. */
	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		if (sz > HW_ETH_MAX_RAW) {
			req->ios2_Req.io_Error = S2ERR_SOFTWARE;
			req->ios2_WireError    = S2WERR_BUFF_ERROR;
			return 1;
		}
		frame_ptr = frame + 4;
		datasize  = sz;
		req->ios2_Req.io_Flags = SANA2IOF_RAW;
	} else {
		if (sz < HW_ETH_HDR_SIZE || sz > HW_ETH_MAX_STD) {
			req->ios2_Req.io_Error = S2ERR_SOFTWARE;
			req->ios2_WireError    = S2WERR_BUFF_ERROR;
			return 1;
		}
		frame_ptr = frame + 4 + HW_ETH_HDR_SIZE;
		datasize  = (ULONG)sz - HW_ETH_HDR_SIZE;   /* ≤ HW_ETH_MTU by the check above */
		req->ios2_Req.io_Flags = 0;
	}

	/* Internal assertion: staging buffer must fit datasize + 2-byte
	 * phase offset. With the protocol caps above, datasize ≤ 1518
	 * (RAW, VLAN-tagged) and RX_STAGE_SIZE is 1536, so this is always
	 * true — the check exists only as a backstop for future MTU /
	 * staging-size changes. */
	if (datasize + 2 > RX_STAGE_SIZE) {
		req->ios2_Req.io_Error = S2ERR_SOFTWARE;
		req->ios2_WireError    = S2WERR_BUFF_ERROR;
		return 1;
	}

	req->ios2_DataLength = datasize;

	bm = (struct BufferManagement *)req->ios2_BufferManagement;
	{
		/* SANA-II contract: bm_CopyToBuffer is a synchronous copy — it
		 * reads `datasize` bytes from `source` into the client-owned
		 * destination and returns success/failure of a completed copy.
		 * It is not a descriptor submission. This routine (and the
		 * pre-rev-20 direct-MMIO path before it) already depended on
		 * that contract: on return we write `rx_accept`, which hands
		 * the backlog slot back to the firmware for reuse by the next
		 * inbound frame. If the callback deferred consumption, the
		 * hardware slot would be overwritten by incoming traffic long
		 * before the client finished copying — so reusing db_RxStage
		 * across successive frames adds no new lifetime assumption
		 * beyond what the SANA-II spec already requires.
		 *
		 * Fast path: stage the payload through Fast RAM so Roadshow's
		 * generic memcpy sees a longword-aligned RAM source and can copy
		 * at memory speed instead of stalling the Zorro bus word by
		 * word. Fallback: if no staging buffer is available (Fast RAM
		 * allocation failed at open time), hand Roadshow the MMIO
		 * source directly — slower, but functionally identical to the
		 * pre-rev-20 behavior. */
		void *copy_src = db->db_RxStage
			? (void *)zznet_mmio_read_block(frame_ptr, db->db_RxStage, datasize)
			: (void *)frame_ptr;

		if (!(*bm->bm_CopyToBuffer)((void*)req->ios2_Data, copy_src, datasize)) {
			req->ios2_Req.io_Error = S2ERR_SOFTWARE;
			req->ios2_WireError    = S2WERR_BUFF_ERROR;
			err = 1;
		} else {
			req->ios2_Req.io_Error = req->ios2_WireError = 0;
		}
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

  USHORT old_serial    = 0;
  BOOL   have_baseline = FALSE;
  ULONG  recv          = Wait(wmask);   /* wait for first packet */

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
      /* Wire-level sanity check on the HW frame size. Reject frames
       * whose size field is shorter than the full ethernet header (14
       * bytes — dst/src MAC + ethertype) or longer than the widest
       * accepted frame (HW_ETH_MAX_RAW, 1518 incl. 802.1Q tag). These
       * are HW- or firmware-level artifacts (torn reads, cold-boot
       * 0xFFFF, corrupt backlog slots) — completing a client read
       * with an error for them would turn line noise into user-visible
       * RX failures.
       *
       * The lower bound is 14, not 18: a frame with sz == 14 has a
       * full ethernet header (ethertype included at bytes 12-13, i.e.
       * frame+16..17 after the 4-byte HW size/serial prefix) and a
       * zero-byte payload. Such frames are legitimate on the wire
       * once the EMAC strips FCS and padding — rejecting them would
       * silently drop valid short control frames from RAW listeners.
       *
       * On the happy path we drop the bad HW frame only: bump
       * BadData, release the backlog slot via rx_accept, leave every
       * pending listener untouched on the read list. */
      if (sz < HW_ETH_HDR_SIZE || sz > HW_ETH_MAX_RAW) {
        global_stats.BadData++;
        have_baseline = TRUE;
        old_serial    = serial;
        *rx_accept = 1;
        continue;
      }

      USHORT packet_type = *(volatile USHORT*)(frm + 16);
      struct IOSana2Req *match = NULL;

      /* Gap detection: the firmware increments 'serial' once per
       * received frame; when the Amiga falls behind and the firmware
       * backlog overflows, frames are dropped
       * at the MAC layer and the next delivered frame's serial skips
       * ahead. A "reasonable" gap is bounded by the backlog depth.
       *
       * Any much larger delta is almost certainly an artifact — a torn
       * header read, an uninitialised-DRAM 0xFFFF on cold boot before
       * the first real frame lands, or a firmware-side reset — not a
       * genuine miss. Count those separately in BadData so Overruns
       * stays trustworthy.
       *
       * Unsigned 16-bit subtraction wraps correctly, so delta also
       * handles the 65535→0 serial rollover. */
      if (have_baseline) {
        USHORT delta = (USHORT)(serial - old_serial);
        if (delta > 1 && delta <= 128) {
          global_stats.Overruns += (ULONG)(delta - 1);
        } else if (delta > 128) {
          /* anomaly — don't pollute Overruns */
          global_stats.BadData++;
        }
      }
      have_baseline = TRUE;
      old_serial    = serial;

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
        ULONG res = read_frame(db, match, frm, sz, packet_type);
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
