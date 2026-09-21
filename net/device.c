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
#include "zzcfg_query.h"
#include "macros.h"

// FIXME get rid of global var!
static ULONG ZZ9K_REGS = 0;
#define ZZ9K_RX 0x2000
#define ZZ9K_TX 0x8000

struct Sana2DeviceStats global_stats;
BOOL is_online;

/* issue #29: count of empty (firmware-cleared) RX slots the framer skipped
 * instead of acking. Surfaced via S2_GETSPECIALSTATS as "RxEmptySlot".
 * After this fix it should be large while BadData/Overruns drop to ~0,
 * confirming the old counts were the empty-slot artifact, not real loss. */
ULONG rxv_empty_slot = 0;
#define ZZSS_RX_EMPTY_SLOT 0x5A5A0021UL

/* issue #29 residual-stall ACK probe (diagnostic).
 *
 * We parse the TCP headers of the inbound (server -> Amiga, src port
 * 445) SMB connection from the exact staged bytes handed to the stack
 * in read_frame: the server's cumulative ACK = how far the server has
 * received the Amiga's upload.
 *
 * Read at a stall: if the server's cumulative ACK (rxv_srv_ack) keeps
 * advancing while the upload still stalls, the ACKs ARE reaching the
 * Amiga — the stall is stack-side send handling, not card loss. (The
 * outbound half of this probe — the Amiga's own send seq — was retired
 * with the synchronous TX path; the stack in use, AmiNetXDuo, does not
 * exhibit the Roadshow retransmit bug this diagnosed.) */
volatile ULONG rxv_srv_ack     = 0;   /* last server cumulative ack (in, src 445) */
volatile ULONG rxv_srv_ack_upd = 0;   /* # times srv ack advanced forward         */
volatile ULONG rxv_p445_in     = 0;   /* inbound TCP frames parsed (src 445)       */
#define ZZSS_RX_SRV_ACK      0x5A5A0022UL
#define ZZSS_RX_SRV_ACK_UPD  0x5A5A0023UL
#define ZZSS_RX_P445_IN      0x5A5A0024UL

/* Minimal, bounds-checked IPv4/TCP header parse over `ip` (the IP header),
 * `len` bytes available. Reads byte-wise through a volatile pointer so it is
 * safe on both RAM (db_RxStage) and MMIO (FPGA TX/RX window) sources and the
 * compiler cannot coalesce a byte-combine into an unaligned wide load (the
 * documented Cortex-A9 strongly-ordered fault applies firmware-side; byte
 * reads are the portable rule for slot memory on both ends). Returns 1 and
 * fills the requested out-params for an IPv4/TCP packet, else 0. */
static int zznet_parse_ip_tcp(volatile const UBYTE *ip, ULONG len,
                              USHORT *sport, USHORT *dport,
                              ULONG *seq, ULONG *ack, ULONG *paylen)
{
	UBYTE  ihl;
	ULONG  ip_hl, tcp_off, total_len, hdrs;
	volatile const UBYTE *tcp;

	if (len < 20)              return 0;   /* room for a min IPv4 header  */
	if ((ip[0] >> 4) != 4)     return 0;   /* IPv4                        */
	ihl   = ip[0] & 0x0f;
	ip_hl = (ULONG)ihl * 4;
	if (ip_hl < 20 || ip_hl > len) return 0;
	if (ip[9] != 6)            return 0;   /* TCP                         */

	total_len = ((ULONG)ip[2] << 8) | ip[3];
	if (total_len > len) total_len = len;  /* clamp to what we actually have */
	if (total_len < ip_hl + 20) return 0;  /* room for a min TCP header   */

	tcp = ip + ip_hl;
	if (sport) *sport = ((USHORT)tcp[0] << 8) | tcp[1];
	if (dport) *dport = ((USHORT)tcp[2] << 8) | tcp[3];
	if (seq)   *seq   = ((ULONG)tcp[4] << 24) | ((ULONG)tcp[5] << 16) |
	                    ((ULONG)tcp[6] << 8)  |  (ULONG)tcp[7];
	if (ack)   *ack   = ((ULONG)tcp[8] << 24) | ((ULONG)tcp[9] << 16) |
	                    ((ULONG)tcp[10] << 8) |  (ULONG)tcp[11];
	tcp_off = (ULONG)(tcp[12] >> 4) * 4;
	if (tcp_off < 20) tcp_off = 20;
	if (paylen) {
		hdrs = ip_hl + tcp_off;
		*paylen = (total_len > hdrs) ? (total_len - hdrs) : 0;
	}
	return 1;
}

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
#define ZZNET_MAX_DELIVER 8

/* One TX staging slot: worst frame (1518) plus phase margin. */
#define ZZNET_TX_STAGE_SIZE 1538

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

          BOOL have_env_mac = FALSE;

          if ((fh=Open("ENV:ZZ9K_MAC",MODE_OLDFILE))) {
            UBYTE char_buf[32];
            char* res = FGets(fh,char_buf,18);
            if (!res || strlen(char_buf)<17) {
              D(("ZZ9000Net: MAC address in ENV:ZZ9K_MAC has invalid syntax.\n"));
            } else {
              D(("ZZ9000Net: Setting MAC address from ENV:ZZ9K_MAC.\n"));
              set_mac_from_string(char_buf);
              have_env_mac = TRUE;
            }
            Close(fh);
          }

          if (have_env_mac) {
            // ENV override wins: push it into the firmware
            // FIXME
            *(volatile USHORT*)(ZZ9K_REGS+0x84) = (HW_MAC[0]<<8)|HW_MAC[1];
            *(volatile USHORT*)(ZZ9K_REGS+0x84) = (HW_MAC[0]<<8)|HW_MAC[1];
            *(volatile USHORT*)(ZZ9K_REGS+0x86) = (HW_MAC[2]<<8)|HW_MAC[3];
            *(volatile USHORT*)(ZZ9K_REGS+0x86) = (HW_MAC[2]<<8)|HW_MAC[3];
            *(volatile USHORT*)(ZZ9K_REGS+0x88) = (HW_MAC[4]<<8)|HW_MAC[5];
          } else {
            // Adopt the firmware's current MAC instead of forcing the
            // old built-in default: honors a `mac` line in ZZ9000.CFG
            // (applied at cold boot, firmware 2.3+) and reads back the
            // same 68:82:F2:00:01:00 default otherwise.
            USHORT mac_hi  = *(volatile USHORT*)(ZZ9K_REGS+0x84);
            USHORT mac_mid = *(volatile USHORT*)(ZZ9K_REGS+0x86);
            USHORT mac_lo  = *(volatile USHORT*)(ZZ9K_REGS+0x88);
            HW_MAC[0] = mac_hi >> 8;
            HW_MAC[1] = mac_hi & 0xff;
            HW_MAC[2] = mac_mid >> 8;
            HW_MAC[3] = mac_mid & 0xff;
            HW_MAC[4] = mac_lo >> 8;
            HW_MAC[5] = mac_lo & 0xff;
            D(("ZZ9000Net: Using firmware MAC.\n"));
          }

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
		UWORD cfg_present = 0;
		if ((fh=Open("ENV:ZZ9K_INT2",MODE_OLDFILE))) {
			D(("ZZ9000Net: Using INT2 mode (ENV).\n"));
			Close(fh);
			db->db_Flags |= DEVF_INT2MODE;
		} else if (ok && ZZ9K_REGS &&
				zzcfg_query(ZZ9K_REGS, ZZ_CFG_KEY_INT2, &cfg_present) && cfg_present) {
			// `int2 = on` in ZZ9000.CFG (firmware 2.3+)
			D(("ZZ9000Net: Using INT2 mode (ZZ9000.CFG).\n"));
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
  struct BufferManagement *bm = NULL;

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
      /* Negotiate the AmiNetXDuo extension tags from the opener's tag
       * list BEFORE ios2_BufferManagement is repointed at our record
       * (zznet_ext.h writes pointer-returns back per anxs2ext.h, and a
       * partial pair disables the extensions exactly as offering no
       * tags). The standard copy hooks keep their GetTagData reads. */
      if (ioreq->ios2_BufferManagement) {
        zznet_ext_negotiate((const struct zznet_tag *)ioreq->ios2_BufferManagement,
                            &bm->bm_Ext);
      }
      bm->bm_CopyToBuffer = (BMFunc)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
      bm->bm_CopyFromBuffer = (BMFunc)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ioreq->ios2_BufferManagement);
      NEWLIST(&bm->bm_ReadList);

      ioreq->ios2_BufferManagement = (VOID *)bm;
      ioreq->ios2_Req.io_Error = 0;
      ioreq->ios2_Req.io_Unit = (struct Unit *)unit;
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
      NEWLIST((struct List*)&db->db_Openers);
      InitSemaphore(&db->db_ReadListSem);
      NEWLIST((struct List*)&db->db_TXList);
      InitSemaphore(&db->db_TXSem);
      /* Reset the file-scope diagnostic counters alongside global_stats so a
       * close/reopen presents a consistent baseline: S2_GETGLOBALSTATS starts
       * from zero here, and S2_GETSPECIALSTATS (RxEmptySlot) must too, else it
       * would report totals accumulated across previous device sessions. */
      rxv_empty_slot = 0;
      rxv_srv_ack = rxv_srv_ack_upd = rxv_p445_in = 0;

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
              /* TX staging slots (KTD3): non-fatal like the RX stage —
               * without them zznet_tx_write sends inline only (still
               * correct, just no parking under bursts). */
              if (!db->db_TxSlots) {
                db->db_TxSlots = AllocVec(ZZNET_TX_SLOTS * ZZNET_TX_STAGE_SIZE,
                                          MEMF_FAST);
                if (db->db_TxSlots && ((ULONG)db->db_TxSlots & 3)) {
                  FreeVec(db->db_TxSlots);
                  db->db_TxSlots = NULL;
                }
                if (!db->db_TxSlots) {
                  D(("ZZ9000Net: TX slots unavailable; inline TX only\n"));
                }
              }
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
    /* Register the opener now that open fully succeeded. */
    ObtainSemaphore(&db->db_ReadListSem);
    AddTail((struct List*)&db->db_Openers, (struct Node*)&bm->bm_Node);
    ReleaseSemaphore(&db->db_ReadListSem);
	}

	if (ret == IOERR_OPENFAIL) {
		/* A failed open's BufferManagement is ours to free (the
		 * secondary-open rejection already freed its own above). */
		if (bm && ioreq->ios2_BufferManagement == (VOID *)bm) {
			FreeVec(bm);
			ioreq->ios2_BufferManagement = NULL;
		}
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
	 * previously this was leaked on every close.
	 *
	 * KTD11 pin rule: a request frame_proc detached for delivery pins the
	 * record (bm_InUse). Close aborts this opener's still-queued reads,
	 * marks the record closing, and frees it immediately only when no
	 * drain holds a pin — otherwise frame_proc's last unpin frees it, so
	 * the drainer never calls hooks from freed memory. */
	/* KTD11: abort this opener's parked writes before its record goes.
	 * The drainer never dereferences the opener record (the copy hook
	 * runs only in the request's own BeginIO, at stage time), so a
	 * parked write only needs its list entry and slot released. */
	{
		struct IOSana2Req *s2 = (struct IOSana2Req *)ioreq;
		struct BufferManagement *bm =
			(struct BufferManagement *)s2->ios2_BufferManagement;
		if (bm) {
			struct IOSana2Req *txr, *txnext;
			int i;
			ObtainSemaphore(&db->db_TXSem);
			for (txr = (struct IOSana2Req *)db->db_TXList.lh_Head;
			     txr->ios2_Req.io_Message.mn_Node.ln_Succ;
			     txr = txnext) {
				txnext = (struct IOSana2Req *)txr->ios2_Req.io_Message.mn_Node.ln_Succ;
				if ((struct BufferManagement *)txr->ios2_BufferManagement != bm)
					continue;
				Remove((struct Node *)txr);
				for (i = 0; i < ZZNET_TX_SLOTS; i++) {
					if (db->db_TxSlotReq[i] == txr)
						db->db_TxSlotReq[i] = NULL;
				}
				txr->ios2_Req.io_Error = IOERR_ABORTED;
				txr->ios2_WireError = 0;
				ReplyMsg((struct Message *)txr);
			}
			ReleaseSemaphore(&db->db_TXSem);
		}
	}

	{
		struct IOSana2Req *s2 = (struct IOSana2Req *)ioreq;
		struct BufferManagement *bm =
			(struct BufferManagement *)s2->ios2_BufferManagement;
		if (bm) {
			struct Node *n, *next;
			int pending;

			ObtainSemaphore(&db->db_ReadListSem);
			Remove((struct Node *)&bm->bm_Node);
			for (n = bm->bm_ReadList.lh_Head; n->ln_Succ; n = next) {
				next = n->ln_Succ;
				Remove(n);
				((struct IORequest *)n)->io_Error = IOERR_ABORTED;
				ReplyMsg((struct Message *)n);
			}
			pending     = bm->bm_InUse;
			bm->bm_Closing = 1;
			ReleaseSemaphore(&db->db_ReadListSem);

			if (!pending) {
				FreeVec(bm);
			}
			s2->ios2_BufferManagement = NULL;
		}
	}

	db->db_Lib.lib_OpenCnt--;

  if (db->db_Lib.lib_OpenCnt == 0) {
    /* Last opener gone: reply-abort every parked write before the
     * worker exits (KTD11) — frame_proc's exit must not strand them. */
    {
      struct IOSana2Req *txr, *txnext;
      ObtainSemaphore(&db->db_TXSem);
      for (txr = (struct IOSana2Req *)db->db_TXList.lh_Head;
           txr->ios2_Req.io_Message.mn_Node.ln_Succ;
           txr = txnext) {
        txnext = (struct IOSana2Req *)txr->ios2_Req.io_Message.mn_Node.ln_Succ;
        Remove((struct Node *)txr);
        txr->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        txr->ios2_WireError = S2WERR_GENERIC_ERROR;
        ReplyMsg((struct Message *)txr);
      }
      ReleaseSemaphore(&db->db_TXSem);
    }
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

    /* Staging buffers are freed only after the worker process has
     * exited above (ObtainSemaphore/ReleaseSemaphore pair). That
     * frame_proc or under db_TXSem against these allocations. */
    if (db->db_RxStage) {
      FreeVec(db->db_RxStage);
      db->db_RxStage = NULL;
    }
    if (db->db_TxSlots) {
      FreeVec(db->db_TxSlots);
      db->db_TxSlots = NULL;
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
static int zznet_tx_write(DEVBASETYPE *db, struct IOSana2Req *req);

ULONG read_frame(DEVBASETYPE *db, struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp, UBYTE *pre_staged);

static void zznet_rx_unpin(DEVBASETYPE *db, struct BufferManagement *bm);
static void zznet_cont_reset_all(DEVBASETYPE *db);
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
      /* Queue on THIS opener's list (KTD2): each opener gets its own
       * delivery of frames for the packet types it tracks; frame_proc
       * walks every opener's list. */
      struct BufferManagement *bm =
          (struct BufferManagement *)ioreq->ios2_BufferManagement;
      ioreq->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
      ObtainSemaphore(&db->db_ReadListSem);
      AddHead((struct List*)&bm->bm_ReadList, (struct Node*)ioreq);
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
    /* KTD3: drain first (BeginIO-entry trigger), then inline-send or
     * park. zznet_tx_write replies the request itself on every path —
     * inline completion, empty write, parked completion by the drainer,
     * and immediate failure — so BeginIO must not DevTermIO it. */
    (void)zznet_tx_write(db, ioreq);
    ioreq = NULL;
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
    /* KTD5: an offline-online transition invalidates every opener's
     * continuation record. */
    zznet_cont_reset_all(db);
    break;
  case S2_OFFLINE:
    is_online = FALSE;
    break;

  case ANXD_CMD_READ_BATCH: {
    /* KTD6: many CMD_READs in one call. ios2_Data is the caller's Exec
     * List of prepared IOSana2Req; queue each onto ITS opener's read
     * list as its own CMD_READ would. Read semaphore at task level
     * first, then Disable around the pure list work only (KTD6: a
     * contested ObtainSemaphore waits, and waiting inside Disable()
     * hangs Exec). The list is emptied either way. */
    struct List *batch = (struct List *)ioreq->ios2_Data;
    if (!batch || !ioreq->ios2_BufferManagement) {
      ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
      ioreq->ios2_WireError = S2WERR_BUFF_ERROR;
      break;
    }
    ObtainSemaphore(&db->db_ReadListSem);
    Disable();
    {
      struct Node *bn, *bnext;
      for (bn = batch->lh_Head; bn->ln_Succ; bn = bnext) {
        struct IOSana2Req *br = (struct IOSana2Req *)bn;
        struct BufferManagement *bbm =
            (struct BufferManagement *)br->ios2_BufferManagement;
        bnext = bn->ln_Succ;
        if (bbm) {
          br->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
          br->ios2_Req.io_Error = S2ERR_NO_ERROR;
          Remove(bn);
          AddHead((struct List *)&bbm->bm_ReadList, bn);
        }
        /* A request without buffer management is dropped from the
         * batch (the list is emptied either way, per the contract). */
        else {
          Remove(bn);
          br->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
          br->ios2_WireError = S2WERR_BUFF_ERROR;
          ReplyMsg((struct Message *)br);
        }
      }
    }
    Enable();
    ReleaseSemaphore(&db->db_ReadListSem);
    break;
  }

  case ANXD_CMD_RX_POLL:
    /* KTD6: the single-frame serial-acknowledge window cannot hold a
     * frame for a late read — withholding the ack stalls ALL reception
     * while the firmware backlog fills. The honest answer is
     * not-supported; the opener stops sending it. */
    ioreq->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
    ioreq->ios2_WireError = S2WERR_GENERIC_ERROR;
    break;

  case ANXD_CMD_RX_CAPACITY:
    /* KTD6: 0 — "no limit worth stating" — until U2's saturation blast
     * proves the firmware does NOT pause the wire; if drops accumulate
     * without pause, this becomes the measured backlog bytes (OQ2). */
    ioreq->ios2_DataLength = 0;
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
      /* issue #29: expose RxEmptySlot — empty (firmware-cleared) RX slots the
       * framer skipped instead of acking. ZZNetStats prints it by name. */
      struct Sana2SpecialStatHeader *s2ssh = (struct Sana2SpecialStatHeader *)ioreq->ios2_StatData;
      if (s2ssh) {
        struct Sana2SpecialStatRecord *rec =
            (struct Sana2SpecialStatRecord *)(s2ssh + 1);
        ULONG max = s2ssh->RecordCountMax;
        ULONG n = 0;
        if (n < max) { rec[n].Type = ZZSS_RX_EMPTY_SLOT; rec[n].Count = rxv_empty_slot;   rec[n].String = (char*)"RxEmptySlot"; n++; }
        if (n < max) { rec[n].Type = ZZSS_RX_P445_IN;    rec[n].Count = rxv_p445_in;      rec[n].String = (char*)"P445In";     n++; }
        if (n < max) { rec[n].Type = ZZSS_RX_SRV_ACK;    rec[n].Count = rxv_srv_ack;      rec[n].String = (char*)"SrvAck";     n++; }
        if (n < max) { rec[n].Type = ZZSS_RX_SRV_ACK_UPD;rec[n].Count = rxv_srv_ack_upd;  rec[n].String = (char*)"SrvAckUpd";  n++; }
        s2ssh->RecordCountSupplied = n;
      }
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
	struct Node* next;
	struct BufferManagement* bm;
	LONG ret = -1;

	D(("ZZ9000Net: AbortIO on %lx\n",(ULONG)ioreq));

	/* Walk every opener's read list under the semaphore to make sure the
	 * IO is still pending (and not already being serviced by frame_proc,
	 * which detaches requests before draining them). Only then is it safe
	 * to Remove()/Reply it; otherwise the caller gets -1 meaning "IO was
	 * not abortable". */
	ObtainSemaphore(&db->db_ReadListSem);
	for (bm = (struct BufferManagement*)db->db_Openers.lh_Head;
	     bm->bm_Node.mln_Succ;
	     bm = (struct BufferManagement*)bm->bm_Node.mln_Succ) {
		for (n = bm->bm_ReadList.lh_Head; n->ln_Succ; n = next) {
			next = n->ln_Succ;
			if (n == (struct Node*)ioreq) {
				Remove(n);
				ret = 0;
				break;
			}
		}
		if (ret == 0) break;
	}
	ReleaseSemaphore(&db->db_ReadListSem);

	/* Parked writes abort too (KTD11); a write inside the TX critical
	 * section (staging or kicking) is off-list and not abortable. */
	if (ret != 0) {
		ObtainSemaphore(&db->db_TXSem);
		for (n = db->db_TXList.lh_Head; n->ln_Succ; n = next) {
			next = n->ln_Succ;
			if (n == (struct Node*)ioreq) {
				int i;
				Remove(n);
				for (i = 0; i < ZZNET_TX_SLOTS; i++) {
					if (db->db_TxSlotReq[i] == (struct IOSana2Req *)ioreq)
						db->db_TxSlotReq[i] = NULL;
				}
				ret = 0;
				break;
			}
		}
		ReleaseSemaphore(&db->db_TXSem);
	}

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

/* Single-copy direct drain (KTD4): MMIO → the opener's answered buffer,
 * accumulating the published ones-complement sum. Source is the FPGA
 * window (volatile, word-aligned reads); dst is opener RAM where
 * unaligned longs are legal on 68020+.
 *
 * Sum positions are relative to the PAYLOAD start, never to the MMIO
 * address: the published contract sums consecutive big-endian
 * longwords of the delivered bytes (bytes 0-3, 4-7, ...), so for
 * 01 02 03 04 05 06 the sum is 0x01020304 + 0x05060000. The payload
 * begins 2 mod 4 in the window, so a longword covering bytes 0-3 can
 * never be one aligned MMIO read; each published longword is assembled
 * from two word reads (each a single Zorro cycle) with end-around
 * carry per longword — exactly n68k_port_in_l_sum's per-longword
 * accumulation, at twice the word-cycle count of an address-aligned
 * walk. That is the price of a sum the opener can trust. */
static ULONG zznet_mmio_read_sum(volatile UBYTE *src, UBYTE *dst, ULONG n)
{
	ULONG sum = 0;

	while (n >= 4) {
		USHORT whi = *(volatile USHORT *)src;
		USHORT wlo = *(volatile USHORT *)(src + 2);
		ULONG v;
		*(USHORT *)dst = whi;
		*(USHORT *)(dst + 2) = wlo;
		v = ((ULONG)whi << 16) | wlo;
		sum += v;
		if (sum < v) sum++;
		src += 4; dst += 4; n -= 4;
	}

	if (n) {
		/* Zero-padded BE tail: the remaining 1-3 bytes occupy the
		 * high bytes of a final longword — a 3-byte tail is
		 * word<<16 | byte<<8, a 2-byte tail word<<16, a lone byte
		 * (1 mod 4 payload) sits at the very top: byte<<24. */
		ULONG v = 0;
		int had_word = 0;
		if (n >= 2) {
			USHORT w = *(volatile USHORT *)src;
			*(USHORT *)dst = w;
			src += 2; dst += 2; n -= 2;
			v |= (ULONG)w << 16;
			had_word = 1;
		}
		if (n) {
			UBYTE b = *src;
			*dst = b;
			v |= (ULONG)b << (had_word ? 8 : 24);
		}
		sum += v;
		if (sum < v) sum++;
	}

	return sum;
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

ULONG read_frame(DEVBASETYPE *db, struct IOSana2Req *req, volatile UBYTE *frame, USHORT sz, USHORT tp, UBYTE *pre_staged)
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
		 * It is not a descriptor submission: on return we write
		 * `rx_accept`, which hands the backlog slot back to the firmware;
		 * if the callback deferred consumption, the hardware slot would
		 * be overwritten long before the client finished copying.
		 *
		 * Source selection (KTD2): when the caller staged this cooked
		 * payload once for all recipients (frame_proc multi-opener
		 * delivery), reuse it instead of paying the MMIO reads per
		 * recipient. Otherwise stage through Fast RAM so the stack's
		 * generic memcpy sees a longword-aligned RAM source; with no
		 * staging buffer available, hand the MMIO source directly —
		 * slower, but functionally identical to the pre-rev-20
		 * behavior. */
		void *copy_src;

		if (pre_staged && !(req->ios2_Req.io_Flags & SANA2IOF_RAW)) {
			copy_src = pre_staged;
		} else if (db->db_RxStage) {
			copy_src = (void *)zznet_mmio_read_block(frame_ptr, db->db_RxStage, datasize);
		} else {
			copy_src = (void *)frame_ptr;
		}

		if (!(*bm->bm_CopyToBuffer)((void*)req->ios2_Data, copy_src, datasize)) {
			req->ios2_Req.io_Error = S2ERR_SOFTWARE;
			req->ios2_WireError    = S2WERR_BUFF_ERROR;
			err = 1;
		} else {
			req->ios2_Req.io_Error = req->ios2_WireError = 0;

			/* issue #29 ACK probe: for the SMB (445) connection, record the
			 * server's cumulative ack from the SAME bytes the stack receives
			 * (copy_src = the staged IP packet, non-RAW). src port 445 =
			 * server -> Amiga. */
			if (tp == 0x0800) {
				USHORT sp = 0, dp = 0;
				ULONG  ack = 0;
				if (zznet_parse_ip_tcp((volatile const UBYTE *)copy_src,
				                       datasize, &sp, &dp, NULL, &ack, NULL) &&
				    sp == 445) {
					rxv_p445_in++;
					if (ack != rxv_srv_ack) {
						if ((LONG)(ack - rxv_srv_ack) > 0)
							rxv_srv_ack_upd++;
						rxv_srv_ack = ack;
					}
				}
			}
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

/* Stage a write's bytes (KTD3): header build for cooked frames, then
 * the opener's copy hook moves the payload — into a TX slot (Fast RAM,
 * for parked writes) or straight into the FPGA window (the inline fast
 * path). A size policy up front rejects oversized lengths before they
 * can overflow either destination. Returns the total frame size
 * (0 = nothing to send), or (USHORT)-1 on rejection/copy failure. */
static USHORT zznet_tx_stage(struct IOSana2Req *req, UBYTE *slot)
{
	struct BufferManagement *bm;
	USHORT sz;

	/* Size policy (mirror of read_frame's): MTU-conformant stacks never
	 * hit this, but a buggy opener's oversized DataLength must not
	 * overflow the 1538-byte staging slot (or the FPGA window). */
	if (req->ios2_DataLength >
	    ((req->ios2_Req.io_Flags & SANA2IOF_RAW) ? (ULONG)HW_ETH_MAX_RAW
	                                             : (ULONG)HW_ETH_MTU)) {
		return (USHORT)-1;
	}

	if (req->ios2_Req.io_Flags & SANA2IOF_RAW) {
		sz = req->ios2_DataLength;
	} else {
		sz = req->ios2_DataLength + HW_ETH_HDR_SIZE;
		/* Build the 14-byte Ethernet header (memcpy lets the compiler
		 * emit move.l where alignment permits). */
		*((USHORT*)(slot + 12)) = (USHORT)req->ios2_PacketType;
		memcpy(slot,     req->ios2_DstAddr, HW_ADDRFIELDSIZE);
		memcpy(slot + 6, HW_MAC,            HW_ADDRFIELDSIZE);
		slot += HW_ETH_HDR_SIZE;
	}

	if (sz == 0) {
		return 0;
	}

	bm = (struct BufferManagement *)req->ios2_BufferManagement;
	if (!(*bm->bm_CopyFromBuffer)(slot, req->ios2_Data, req->ios2_DataLength)) {
		return (USHORT)-1;
	}
	return sz;
}

/* Copy a staged frame into the FPGA TX window (when the frame waits in
 * a Fast-RAM slot), kick, and read the status back — the one MMIO
 * cycle pair the caller no longer waits on. `slot == NULL` means the
 * frame is ALREADY in the window (the inline path staged it there
 * directly): no self-copy pass, which would both waste a full-frame
 * MMIO read/write round-trip and rely on TX-window readback the
 * firmware does not promise. Caller holds db_TXSem (KTD3's single
 * critical section). Returns the hardware status (0 = accepted). */
static ULONG zznet_tx_kick(DEVBASETYPE *db, const UBYTE *slot, USHORT sz)
{
	volatile USHORT *reg;
	ULONG n = sz;
	ULONG rc;

	if (slot) {
		const volatile UBYTE *src = (const volatile UBYTE *)slot;
		volatile UBYTE *dst = (volatile UBYTE *)(ZZ9K_REGS + ZZ9K_TX);
		/* RAM → MMIO copy: longword body, byte tail. Phase is
		 * matched by construction — slot and window are both
		 * longword-aligned at frame start. */
		while (n >= 4) {
			*(volatile ULONG *)dst = *(const volatile ULONG *)src;
			dst += 4; src += 4; n -= 4;
		}
		while (n--) {
			*dst++ = *src++;
		}
	}

	reg = (volatile USHORT *)(ZZ9K_REGS + 0x80);
	*reg = sz;      /* kick the TX engine */
	rc  = *reg;     /* read back hardware status */
	if (rc) {
		D(("tx err: %ld\n", (LONG)rc));
	}
	return rc;
}

/* Complete a sent write: status into the request's error fields (AE3),
 * wire counter, reply with DevTermIO's QUICK semantics — a caller that
 * submitted via DoIO left IOF_QUICK set and skips WaitIO, so an
 * unconditional ReplyMsg would strand the reply on its port. */
static void zznet_tx_complete(DEVBASETYPE *db, struct IOSana2Req *req,
                              ULONG rc)
{
	if (rc != 0) {
		req->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
		req->ios2_WireError    = S2WERR_GENERIC_ERROR;
	} else {
		req->ios2_Req.io_Error = 0;
		req->ios2_WireError    = 0;
		global_stats.PacketsSent++;
	}
	DevTermIO(db, (struct IORequest *)req);
}

/* Drain parked writes: feed the window from TX slots while the list is
 * non-empty (KTD3). Runs under db_TXSem from the posting task, from
 * frame_proc's RX wake, and from the timer liveness trigger. */
static void zznet_tx_drain(DEVBASETYPE *db)
{
	struct IOSana2Req *req;
	int i;

	ObtainSemaphore(&db->db_TXSem);
	while ((req = (struct IOSana2Req *)db->db_TXList.lh_Head)->ios2_Req.io_Message.mn_Node.ln_Succ) {
		/* Find (and free) this request's staging slot. */
		UBYTE *slot = NULL;
		for (i = 0; i < ZZNET_TX_SLOTS; i++) {
			if (db->db_TxSlotReq[i] == req) {
				slot = db->db_TxSlots + (ULONG)i * ZZNET_TX_STAGE_SIZE;
				db->db_TxSlotReq[i] = NULL;
				break;
			}
		}
		if (!slot) {
			/* Slotless park (should not happen): complete with an
			 * error rather than spin forever. */
			Remove((struct Node *)req);
			zznet_tx_complete(db, req, 1);
			continue;
		}
		{
			USHORT sz = (req->ios2_Req.io_Flags & SANA2IOF_RAW)
				? req->ios2_DataLength
				: req->ios2_DataLength + HW_ETH_HDR_SIZE;
			ULONG rc = zznet_tx_kick(db, slot, sz);
			Remove((struct Node *)req);
			zznet_tx_complete(db, req, rc);
		}
	}
	ReleaseSemaphore(&db->db_TXSem);
}

/* CMD_WRITE entry (KTD3). Drain first (the BeginIO-entry trigger), then
 * either send inline when the pipe is empty — the opener's copy hook
 * writes straight into the FPGA window, exactly one payload copy like
 * the old synchronous path — or stage into a free slot and park.
 * Returns 0 when the request was sent or parked, nonzero on immediate
 * failure (the request is already completed with an error). */
static int zznet_tx_write(DEVBASETYPE *db, struct IOSana2Req *req)
{
	int rc = 0;
	int empty;

	zznet_tx_drain(db);

	ObtainSemaphore(&db->db_TXSem);
	empty = !db->db_TXList.lh_Head->ln_Succ ? 1 : 0;
	if (empty || !db->db_TxSlots) {
		/* Fast path: header + payload copy straight into the TX
		 * window (one payload copy, the old path's cost — and the
		 * no-slots fallback for Fast-RAM-less machines), then kick
		 * and complete. Runs under the sem so the window copy
		 * cannot interleave with a drain. */
		UBYTE *window = (UBYTE *)(ZZ9K_REGS + ZZ9K_TX);
		USHORT sz = zznet_tx_stage(req, window);
		if (sz == (USHORT)-1) {
			zznet_tx_complete(db, req, 1);
			rc = 1;
		} else if (sz == 0) {
			/* Nothing to send: complete immediately, no error. */
			req->ios2_Req.io_Error = 0;
			req->ios2_WireError = 0;
			DevTermIO(db, (struct IORequest *)req);
			rc = 0;
		} else {
			/* Frame already in the window: kick-only (NULL src
			 * skips the self-copy). */
			ULONG st = zznet_tx_kick(db, NULL, sz);
			zznet_tx_complete(db, req, st);
		}
	} else {
		/* Pipe busy: park into a free staging slot. No free slot
		 * means the burst outran four slots — fail this write
		 * rather than block BeginIO (the old NO_RESOURCES error). */
		int i;
		for (i = 0; i < ZZNET_TX_SLOTS; i++) {
			if (!db->db_TxSlotReq[i]) break;
		}
		if (i == ZZNET_TX_SLOTS) {
			zznet_tx_complete(db, req, 1);
			rc = 1;
		} else {
			USHORT sz = zznet_tx_stage(req, db->db_TxSlots + (ULONG)i * ZZNET_TX_STAGE_SIZE);
			if (sz == (USHORT)-1) {
				zznet_tx_complete(db, req, 1);
				rc = 1;
			} else if (sz == 0) {
				req->ios2_Req.io_Error = 0;
				req->ios2_WireError = 0;
				DevTermIO(db, (struct IORequest *)req);
			} else {
				db->db_TxSlotReq[i] = req;
				req->ios2_Req.io_Flags &= ~SANA2IOF_QUICK;
				AddTail((struct List *)&db->db_TXList, (struct Node *)req);
				rc = 0; /* parked: completion is asynchronous */
			}
		}
	}
	ReleaseSemaphore(&db->db_TXSem);

	return rc;
}

/* Unpin an opener record after a delivered request (KTD11): always
 * decrement; free only a closing record whose last pin just dropped. */
static void zznet_rx_unpin(DEVBASETYPE *db, struct BufferManagement *bm)
{
	ObtainSemaphore(&db->db_ReadListSem);
	if (--bm->bm_InUse == 0 && bm->bm_Closing) {
		ReleaseSemaphore(&db->db_ReadListSem);
		FreeVec(bm);
	} else {
		ReleaseSemaphore(&db->db_ReadListSem);
	}
}

/* KTD5: invalidate every opener's continuation record — run on a serial
 * gap/overrun and on the offline-online transition. */
static void zznet_cont_reset_all(DEVBASETYPE *db)
{
	struct BufferManagement *cbm;
	ObtainSemaphore(&db->db_ReadListSem);
	for (cbm = (struct BufferManagement *)db->db_Openers.lh_Head;
	     cbm->bm_Node.mln_Succ;
	     cbm = (struct BufferManagement *)cbm->bm_Node.mln_Succ) {
		cbm->bm_Cont.c_valid = 0;
	}
	ReleaseSemaphore(&db->db_ReadListSem);
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

  /* TX liveness timer (KTD3): with no TX-done interrupt, a one-shot
   * timer wake re-checks the window whenever parked writes wait, so a
   * reply-pacing stack or a silent peer cannot deadlock on the final
   * parked request. */
  struct MsgPort   *tport = NULL;
  struct timerequest *treq = NULL;
  ULONG             tmask = 0;
  BOOL              tarmed = FALSE;

  if ((tport = CreateMsgPort())) {
    tmask = 1UL << tport->mp_SigBit;
    if ((treq = (struct timerequest *)CreateIORequest(
             tport, sizeof(struct timerequest)))) {
      if (OpenDevice(TIMERNAME, UNIT_MICROHZ, treq, 0)) {
        DeleteIORequest(treq);
        treq = NULL;
      }
    }
    if (!treq) {
      DeleteMsgPort(tport);
      tport = NULL;
      tmask = 0;
    }
  }

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

    /* issue #29: an all-zero header is an EMPTY (firmware-cleared) slot, not
     * a frame. The firmware zeroes a slot when it has no frame for it, and a
     * real frame always has serial >= 2 (the firmware's frame_serial counter
     * skips both 0 and 1 — reserved for the empty-slot sentinel and the
     * legacy bare-advance ack — so neither is ever assigned to a frame) and
     * size >= 14. Reading a 0 header happens
     * routinely at the backlog drain boundary.
     *
     * We must NOT ack it (*rx_accept). Doing so races a frame landing in this
     * same slot between our read and the ack: ethernet_receive_frame would
     * then consume that real frame (advance + clear the slot) without us ever
     * reading its payload — a silent inbound loss, invisible to the firmware
     * (its frames_dropped stays 0). Treat an empty slot exactly like "nothing
     * new": re-arm the IRQ and wait, leaving old_serial untouched so the next
     * real frame's gap detection is not poisoned. */
    if (sz == 0 && serial == 0) {
      rxv_empty_slot++;
      *irq_ctrl = 1;
      recv = Wait(wmask);
      continue;
    }

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
        /* Ack with the frame's serial so the firmware RX-accept handshake
         * advances past exactly this (bad) frame and nothing else. */
        *rx_accept = serial;
        continue;
      }

      USHORT packet_type = *(volatile USHORT*)(frm + 16);


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
       * Unsigned 16-bit subtraction gives the forward distance directly.
       * The firmware skips BOTH serial 0 and 1 on its u16 wraparound (0 is
       * the empty-slot sentinel, 1 the legacy bare-advance ack), so real
       * serials run 2..0xffff and a clean 0xffff→2 wrap step has a raw delta
       * of 3, not 1. When the serial wrapped (serial < old_serial), discount
       * those two skipped sentinels so the wrap itself is not miscounted as
       * dropped frames. */
      if (have_baseline) {
        USHORT delta = (USHORT)(serial - old_serial);
        if (serial < old_serial)   /* wrapped past the skipped 0 and 1 sentinels */
          delta -= 2;
        if (delta > 1 && delta <= 128) {
          global_stats.Overruns += (ULONG)(delta - 1);
        } else if (delta > 128) {
          /* anomaly — don't pollute Overruns */
          global_stats.BadData++;
        }

        /* KTD5: a serial gap or overrun invalidates every opener's
         * continuation record — a later CONTINUES would chain segments
         * across a loss boundary. */
        if (delta > 1) {
          zznet_cont_reset_all(db);
        }
      }
      have_baseline = TRUE;
      old_serial    = serial;

      /* KTD2 delivery: every opener tracking this packet type gets the
       * frame. Detach one matching read per opener under the semaphore
       * (pinning each record, KTD11), then copy and reply outside it so
       * DevAbortIO / CMD_READ stay unblocked for the duration of the
       * Zorro bus copy. The cooked payload is staged from MMIO ONCE and
       * reused by every cooked recipient. */
      {
        struct BufferManagement *bm;
        struct BufferManagement *mbs[ZZNET_MAX_DELIVER];
        struct IOSana2Req      *reqs[ZZNET_MAX_DELIVER];
        int nmatch = 0, i;
        int first_batch = 1;
        int wire_ok = 0;

        /* Batches of ZZNET_MAX_DELIVER — a fixed stack, but NO ceiling:
         * a full batch means more openers may match behind it, so the
         * do/while keeps collecting (and delivering) until a partial
         * batch ends the pass. Every matching opener is served; the
         * single-taker direct claim only applies when the FIRST batch
         * collected exactly one (that is the total). */
        db->db_DeliverGen++; /* once per wire frame, before all batches */
        do {
        nmatch = 0;
        ObtainSemaphore(&db->db_ReadListSem);
        for (bm = (struct BufferManagement *)db->db_Openers.lh_Head;
             bm->bm_Node.mln_Succ && nmatch < ZZNET_MAX_DELIVER;
             bm = (struct BufferManagement *)bm->bm_Node.mln_Succ) {
          if (bm->bm_ServedGen == db->db_DeliverGen) {
            continue; /* already served this frame by an earlier batch */
          }
          for (ior = (struct IOSana2Req *)bm->bm_ReadList.lh_Head;
               ior->ios2_Req.io_Message.mn_Node.ln_Succ;
               ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
            if (ior->ios2_PacketType == packet_type) {
              Remove((struct Node *)ior);
              mbs[nmatch]  = bm;
              reqs[nmatch] = ior;
              nmatch++;
              bm->bm_InUse++;
              bm->bm_ServedGen = db->db_DeliverGen;
              break; /* one delivery per opener per frame */
            }
          }
        }
        ReleaseSemaphore(&db->db_ReadListSem);

        if (nmatch == 0 && first_batch) {
          /* No listener matched on the first (only) collection pass —
           * frame dropped. A future change could route these to
           * S2_READORPHAN requests. A FULL final batch (exact multiple
           * of ZZNET_MAX_DELIVER) also produces a final empty
           * iteration, which must NOT count: every opener was served. */
          global_stats.UnknownTypesReceived++;
        } else if (first_batch && nmatch == 1 &&
                   !(reqs[0]->ios2_Req.io_Flags & SANA2IOF_RAW) &&
                   sz >= HW_ETH_HDR_SIZE && sz <= HW_ETH_MAX_STD &&
                   zznet_ext_can_claim(1, 0, &mbs[0]->bm_Ext)) {
          /* Single-copy direct delivery (KTD4). Validation ran before
           * the claim (wire bounds above, cooked size policy here), the
           * request is unlinked and pinned, and the opener offered the
           * pair without a filter: ask where the payload should land
           * and drain the window straight into it. */
          struct IOSana2Req *req = reqs[0];
          struct zznet_ext  *xe  = &mbs[0]->bm_Ext;
          ULONG plen = (ULONG)sz - HW_ETH_HDR_SIZE;
          UBYTE *dst = ((AnxdS2RxDirect)xe->xe_RxDirect)(req->ios2_Data, plen);

          if (dst) {
            ULONG sum;
            ULONG m0 = *(volatile ULONG *)(frm + 4);
            ULONG m1 = *(volatile ULONG *)(frm + 8);
            ULONG m2 = *(volatile ULONG *)(frm + 12);

            /* The link header, written at the 14 bytes the opener
             * reserved before the payload, from the header longwords
             * already read (three bus cycles, not fourteen byte reads),
             * with the ethertype from the already-read packet_type. */
            if (xe->xe_RxLinkHdr) {
              UBYTE *h = dst - HW_ETH_HDR_SIZE;
              h[0]  = (UBYTE)(m0 >> 24); h[1]  = (UBYTE)(m0 >> 16);
              h[2]  = (UBYTE)(m0 >> 8);  h[3]  = (UBYTE)m0;
              h[4]  = (UBYTE)(m1 >> 24); h[5]  = (UBYTE)(m1 >> 16);
              h[6]  = (UBYTE)(m1 >> 8);  h[7]  = (UBYTE)m1;
              h[8]  = (UBYTE)(m2 >> 24); h[9]  = (UBYTE)(m2 >> 16);
              h[10] = (UBYTE)(m2 >> 8);  h[11] = (UBYTE)m2;
              h[12] = (UBYTE)(packet_type >> 8);
              h[13] = (UBYTE)packet_type;
            }

            sum = zznet_mmio_read_sum(frm + 4 + HW_ETH_HDR_SIZE, dst, plen);

            /* Request fields exactly as the staging path sets them. */
            {
              USHORT *wd = (USHORT *)req->ios2_DstAddr;
              USHORT *ws = (USHORT *)req->ios2_SrcAddr;
              wd[0] = (USHORT)(m0 >> 16);
              wd[1] = (USHORT)(m0 & 0xFFFF);
              wd[2] = (USHORT)(m1 >> 16);
              ws[0] = (USHORT)(m1 & 0xFFFF);
              ws[1] = (USHORT)(m2 >> 16);
              ws[2] = (USHORT)(m2 & 0xFFFF);
              req->ios2_Req.io_Flags = 0;
              if (m0 == 0xFFFFFFFFUL && (m1 & 0xFFFF0000UL) == 0xFFFF0000UL) {
                req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
              }
            }
            req->ios2_PacketType = packet_type;
            req->ios2_DataLength = plen;
            req->ios2_Req.io_Error = req->ios2_WireError = 0;

            /* VERIFIED/CONTINUES from the delivered bytes (now in Fast
             * RAM at dst), gated by the negotiated intersection (KTD5).
             * An opener that negotiated SUMMED-only verifies itself and
             * skips the checksum pass entirely. */
            {
                UBYTE delivered = ANXD_S2_RXF_SUMMED;
                if (xe->xe_RxFlags & (ANXD_S2_RXF_VERIFIED |
                                      ANXD_S2_RXF_CONTINUES)) {
                    UBYTE earned = zznet_rx_flags(dst, plen,
                                                   &mbs[0]->bm_Cont, 1);
                    delivered |= (UBYTE)(earned & (xe->xe_RxFlags &
                                     (ANXD_S2_RXF_VERIFIED |
                                      ANXD_S2_RXF_CONTINUES)));
                }
                ((AnxdS2RxFilled)xe->xe_RxFilled)(req->ios2_Data, plen,
                                                  sum, delivered);
            }
            ReplyMsg((struct Message *)req);

            zznet_rx_unpin(db, mbs[0]);

            wire_ok = 1;
          } else {
            /* Claim declined: fall through to staging with the request
             * already collected — read_frame delivers it (KTD4). */
            goto staging;
          }
        } else {
        staging: {
          UBYTE *staged = NULL;
          int any_ok = 0;
          int any_cooked = 0;

          /* Lazy staging (KTD8/perf): pass the shared staged payload only
           * when at least one cooked recipient exists, and deliver cooked
           * recipients BEFORE raw ones — a raw read_frame re-stages the
           * full frame into db_RxStage, which would clobber the payload
           * image a later cooked recipient still needs. */
          for (i = 0; i < nmatch; i++) {
            if (!(reqs[i]->ios2_Req.io_Flags & SANA2IOF_RAW)) {
              any_cooked = 1;
              break;
            }
          }
          if (any_cooked && db->db_RxStage && sz > HW_ETH_HDR_SIZE) {
            staged = zznet_mmio_read_block(
                frm + 4 + HW_ETH_HDR_SIZE, db->db_RxStage,
                (ULONG)sz - HW_ETH_HDR_SIZE);
          }

          /* Cooked recipients first (shared staged payload), raw after. */
          for (i = 0; i < nmatch * 2; i++) {
            int idx = (i < nmatch)
                ? i                       /* first pass: cooked */
                : (i - nmatch);           /* second pass: raw */
            ULONG res;
            if ((i < nmatch) ==
                ((reqs[idx]->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)) {
              continue; /* wrong pass for this recipient */
            }
            res = read_frame(db, reqs[idx], frm, sz, packet_type, staged);
            if (res == 0) {
              any_ok = 1;
            } else {
              /* read_frame already set io_Error/ios2_WireError; reply so
               * the caller learns the request failed instead of leaving
               * it on a now-dangling list entry. */
              D(("RERR %ld\n", res));
            }
            ReplyMsg((struct Message *)reqs[idx]);

            /* A staged delivery is the opener's immediately-preceding
             * frame: its continuation record cannot describe it (no
             * verified direct drain), so the next CONTINUES must not
             * chain across it (KTD5 / the published contract). */
            mbs[idx]->bm_Cont.c_valid = 0;

            /* Unpin (KTD11): a closing opener's record is freed when the
             * last pinned request is replied. */
            zznet_rx_unpin(db, mbs[idx]);
          }

          if (any_ok) {
            wire_ok = 1;
          }
        }
        }

        first_batch = 0;
        } while (nmatch == ZZNET_MAX_DELIVER);

        /* Wire-level counter: once per wire frame, not per delivery
         * (KTD2), so ZZNetStats attribution stays frames. */
        if (wire_ok) {
          global_stats.PacketsReceived++;
        }
      }

      /* Release the FPGA RX slot so the next frame can land. We ack with the
       * frame's own serial so the firmware RX-accept handshake advances past
       * exactly the frame we just read (a stray ack of an empty/other slot is
       * rejected firmware-side). We do NOT re-enable the ethernet IRQ here —
       * staying masked lets us drain any already-queued frames via the serial
       * recheck on the next loop iteration without paying for an IRQ we'd
       * ignore anyway. */
      *rx_accept = serial;
    } else {
      /* Nothing new. Re-enable the ethernet IRQ, arm the TX liveness
       * timer when parked writes wait (KTD3), then sleep. */
      *irq_ctrl = 1;
      if (treq && !tarmed && db->db_TXList.lh_Head->ln_Succ) {
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs  = 0;
        treq->tr_time.tv_micro = 1000; /* 1 ms: well under a wire time */
        SendIO(treq);
        tarmed = TRUE;
      }
      recv = Wait(wmask | tmask);
      if (recv & tmask) {
        if (tarmed && treq) {
          GetMsg(tport);
          tarmed = FALSE;
        }
        zznet_tx_drain(db);
      }
    }
  }
  // disable interrupt
  *(volatile USHORT*)(ZZ9K_REGS+0x04) = 0;

  /* Timer teardown: abort an outstanding request so the port drains. */
  if (treq) {
    if (tarmed) {
      AbortIO(treq);
      WaitIO(treq);
    }
    CloseDevice(treq);
    DeleteIORequest(treq);
  }
  if (tport) DeleteMsgPort(tport);

  Forbid();
  ReleaseSemaphore(&db->db_ProcExitSem);
}
