/*
 * MNT ZZ9000 Network Driver (ZZ9000Net.device)
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 * Copyright (C) 2018 Henryk Richter <henryk.richter@gmx.net>
 *
 * 2026 GCC port: Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

/*
  device.h

  (C) 2018 Henryk Richter <henryk.richter@gmx.net>

  Device Functions and Definitions


*/
#ifndef _INC_DEVICE_H
#define _INC_DEVICE_H

/* defaults */
#define MAX_UNITS 4

/* TX staging slots (KTD3): parked writes wait here until the drainer
 * feeds the single FPGA TX window. Four covers a TCP burst with the
 * stack's typical write-ahead. */
#define ZZNET_TX_SLOTS 4

/* includes */
#include "compiler.h"
#include <dos/dos.h>
#include <exec/lists.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/semaphores.h>
#include "debug.h"
#include "sana2.h"
#include "zznet_ext.h"

/* reassign Library bases from global definitions to own struct */
#define SysBase       db->db_SysBase
#define DOSBase       db->db_DOSBase
#define UtilityBase   db->db_UtilityBase
#define ExpansionBase db->db_ExpansionBase

struct DevUnit {
	/* HW Data (generic for now) (example only, unused in construct)*/
	ULONG	du_hwl0;
	ULONG	du_hwl1;
	ULONG	du_hwl2;
	APTR	du_hwp0;
	APTR	du_hwp1;
	APTR	du_hwp2;
};

#define DEVF_INT2MODE		(1L << 0)

struct devbase {
	struct Library db_Lib;
	BPTR db_SegList; /* from Device Init */

	ULONG db_Flags;   /* misc */
	struct Library *db_SysBase; /* Exec Base */
	struct Library *db_DOSBase;
	struct Library *db_UtilityBase;
	struct Library *db_ExpansionBase;
	struct Interrupt *db_interrupt;

	/* Device-wide list of open BufferManagement records (one per opener,
	 * struct MinNode bm_Node). Reads queue on the opener's own
	 * bm_ReadList; the single semaphore below guards db_Openers and every
	 * bm_ReadList. */
	struct List db_Openers;
	struct SignalSemaphore db_ReadListSem;
	struct Process* db_Proc;
	struct SignalSemaphore db_ProcExitSem;

	/* Asynchronous TX (KTD3). One critical section (db_TXSem) covers
	 * window-check → payload copy → kick → status read in every
	 * context. db_TXList holds parked CMD_WRITEs whose payloads wait
	 * in db_TxSlots; db_TxSlotReq[i] names the request occupying slot
	 * i (NULL = free). The one-shot timer wake lives in frame_proc. */
	struct List db_TXList;
	struct SignalSemaphore db_TXSem;
	UBYTE *db_TxSlots;
	struct IOSana2Req *db_TxSlotReq[ZZNET_TX_SLOTS];

	/* RX payload staging buffer (see RX_STAGE_SIZE in device.c). Lifetime
	 * is tied to this device base: allocated on first open before the HW
	 * IRQ is enabled, freed on last close after the worker process has
	 * exited. Kept here (instead of file-static) so ownership and
	 * lifetime are bound to the devbase the RX path dereferences. */
	UBYTE *db_RxStage;

	struct DevUnit db_Units[MAX_UNITS]; /* unused in construct */
};

#ifndef DEVBASETYPE
#define DEVBASETYPE struct devbase
#endif
#ifndef DEVBASEP
#define DEVBASEP DEVBASETYPE *db
#endif

/* PROTOS */

ASM LONG LibNull( void );

ASM SAVEDS struct Device *DevInit(ASMR(d0) DEVBASEP                  ASMREG(d0), 
                                  ASMR(a0) BPTR seglist              ASMREG(a0), 
				  ASMR(a6) struct Library *_SysBase  ASMREG(a6) );

ASM SAVEDS LONG DevOpen( ASMR(a1) struct IOSana2Req *ios2            ASMREG(a1), 
                         ASMR(d0) ULONG unit                         ASMREG(d0), 
                         ASMR(d1) ULONG flags                        ASMREG(d1),
                         ASMR(a6) DEVBASEP                           ASMREG(a6) );

ASM SAVEDS BPTR DevClose(   ASMR(a1) struct IORequest *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS BPTR DevExpunge( ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS VOID DevBeginIO( ASMR(a1) struct IOSana2Req *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

ASM SAVEDS LONG DevAbortIO( ASMR(a1) struct IORequest *ios2         ASMREG(a1),
                            ASMR(a6) DEVBASEP                        ASMREG(a6) );

void DevTermIO( DEVBASETYPE*, struct IORequest * );

/* private functions */
#ifdef DEVICE_MAIN

//static void dbNewList( struct List * );
//static LONG dbIsInList( struct List *, struct Node * );

#endif /* DEVICE_MAIN */

#define HW_ADDRFIELDSIZE 6
#define HW_ETH_HDR_SIZE          14       /* ethernet header: dst, src, type */
#define HW_ETH_MTU               1500
#define HW_ETH_VLAN_TAG          4        /* 802.1Q tag adds 4 bytes */
/* Untagged Ethernet frame without FCS, used as the non-RAW size ceiling
 * because the driver advertises MTU = 1500 to SANA-II clients. */
#define HW_ETH_MAX_STD           (HW_ETH_HDR_SIZE + HW_ETH_MTU)              /* 1514 */
/* VLAN-tagged Ethernet frame without FCS, used as the RAW / wire-level
 * size ceiling. Frames up to 802.1Q size are legitimate on tagged links
 * and must be accepted by RAW consumers (packet capture, bridging). */
#define HW_ETH_MAX_RAW           (HW_ETH_MAX_STD + HW_ETH_VLAN_TAG)          /* 1518 */

typedef BOOL (*BMFunc)(void* a __asm("a0"), void* b __asm("a1"), long c __asm("d0"));

typedef struct BufferManagement
{
  struct MinNode   bm_Node;            /* db_Openers linkage                 */
  BMFunc           bm_CopyFromBuffer;
  BMFunc           bm_CopyToBuffer;
  /* AmiNetXDuo extension negotiation (anxs2ext.h via zznet_ext.h) */
  struct zznet_ext bm_Ext;             /* hooks, flags, filter                */
  struct zznet_cont bm_Cont;           /* previous delivered TCP (KTD5)       */
  struct List      bm_ReadList;        /* this opener's posted CMD_READs      */
  /* Drain pinning (KTD11): a request detached for delivery pins this
   * record; a close that races the drain marks bm_Closing and defers the
   * free to the last unpin, so the drainer never touches freed hooks. */
  UWORD            bm_InUse;
  UWORD            bm_Closing;
} BufferManagement;

struct HWFrame {
   USHORT   hwf_Size;
   /* use layout of ethernet header here */
   UBYTE    hwf_DstAddr[HW_ADDRFIELDSIZE];
   UBYTE    hwf_SrcAddr[HW_ADDRFIELDSIZE];
   USHORT   hwf_Type;
   /*UBYTE    hwf_Data[MTU];*/
};

struct InitTable
{
  ULONG LibBaseSize;
  APTR  FunctionTable;
  APTR  DataTable;
  APTR  InitLibTable;
};

#endif /* _INC_DEVICE_H */
