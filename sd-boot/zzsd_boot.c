/*
 * ZZ9000 SD-card boot driver — RDB scan, FSHD segment-list
 * relocation, and partition bootnode registration.
 *
 * Copyright (C) 2026, MNT Research GmbH
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <libraries/expansion.h>
#include <resources/filesysres.h>
#include <dos/doshunks.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <string.h>
#include <stdint.h>
#include "zzsd_cmd.h"

extern struct ExecBase* SysBase;

/* Local debug — re-enables the two helpers just in this file without
 * pulling them into all translation units. Cheap: ~6 insns per call. */
static void dbg_str(void *regs, const char *s) {
    volatile uint16_t *reg = (volatile uint16_t *)((uint8_t *)regs + 0xF0);
    while (*s) { *reg = (uint8_t)*s++; }
}
static void dbg_hex(void *regs, uint32_t v) {
    volatile uint16_t *reg = (volatile uint16_t *)((uint8_t *)regs + 0xF2);
    *reg = (uint16_t)(v >> 16);
    *reg = (uint16_t)v;
}
#undef debugstr
#undef debughex
#define debugstr(r, s) dbg_str((r), (s))
#define debughex(r, v) dbg_hex((r), (v))

static void my_NewList(struct List *l) {
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)l;
}

/* Last reason fsrelocate_simple bailed, 0 == success, for diag printing. */
static uint32_t fsreloc_last_err = 0;
static uint32_t fsreloc_last_idx = 0;
static uint32_t fsreloc_last_cur_hunk = 0;

static APTR fsrelocate_simple(uint8_t *data, uint32_t size) {
    uint32_t *ptr = (uint32_t *)data;
    uint32_t *hunk_ptrs[16];
    uint32_t hunk_sizes[16];
    uint32_t num_hunks;
    int32_t first_hunk;
    int32_t last_hunk;
    uint32_t idx;
    uint32_t cur_hunk;
    uint32_t h;

    for (h = 0; h < 16; h++) {
        hunk_ptrs[h] = 0;
        hunk_sizes[h] = 0;
    }

    fsreloc_last_err = 0;

    if (size < 20) { fsreloc_last_err = 0x11; return NULL; }
    if (ptr[0] != HUNK_HEADER) { fsreloc_last_err = 0x12; return NULL; }
    if (ptr[1] != 0) { fsreloc_last_err = 0x13; return NULL; }

    num_hunks = ptr[2];
    if (num_hunks == 0) { fsreloc_last_err = 0x14; return NULL; }

    first_hunk = (int32_t)ptr[3];
    last_hunk = (int32_t)ptr[4];
    if (first_hunk < 0 || last_hunk < 0 || first_hunk > last_hunk) { fsreloc_last_err = 0x15; return NULL; }

    num_hunks = (uint32_t)(last_hunk - first_hunk + 1);
    if (num_hunks > 16) { fsreloc_last_err = 0x16; return NULL; }

    idx = 5;
    for (h = 0; h < num_hunks; h++) {
        uint32_t hsize = ptr[idx++];
        uint32_t mflags = MEMF_PUBLIC;
        uint32_t *hunk;

        if ((hsize & 0xC0000000UL) == 0xC0000000UL) {
            idx++;
        } else if (hsize & 0x40000000UL) {
            mflags |= MEMF_CHIP;
        }

        hsize &= 0x3FFFFFFFUL;
        hunk_sizes[h] = hsize;
        hunk = AllocMem((hsize + 2) * sizeof(uint32_t), mflags | MEMF_CLEAR);
        if (!hunk) { fsreloc_last_err = 0x17; goto fail; }

        /* Amiga SegList convention: the LONG at BPTR-4 is the size of
         * the allocated block in BYTES (so UnLoadSeg can call FreeMem
         * with the right size). hsize is the hunk size in LONGS from
         * the HUNK_HEADER table; add 2 longs for the size + next-BPTR
         * headers, then convert to bytes. */
        hunk[0] = (hsize + 2) * sizeof(uint32_t);
        hunk[1] = 0;
        hunk_ptrs[h] = hunk + 2;
    }

    for (h = 0; h + 1 < num_hunks; h++) {
        hunk_ptrs[h][-1] = MKBADDR(hunk_ptrs[h + 1] - 1);
    }

    cur_hunk = 0;
    while (idx < (size / 4)) {
        uint32_t htype = ptr[idx++];

        switch (htype) {
        case HUNK_CODE:
        case HUNK_DATA:
        {
            uint32_t hsize;
            if (cur_hunk >= num_hunks) { fsreloc_last_err = 0x18; goto fail; }
            hsize = ptr[idx++];
            if (hsize > hunk_sizes[cur_hunk]) { fsreloc_last_err = 0x19; goto fail; }
            CopyMem(&ptr[idx], hunk_ptrs[cur_hunk], hsize * sizeof(uint32_t));
            idx += hsize;
            cur_hunk++;
            break;
        }
        case HUNK_BSS:
            if (cur_hunk >= num_hunks) { fsreloc_last_err = 0x1A; goto fail; }
            idx++;
            cur_hunk++;
            break;
        case HUNK_RELOC32:
        {
            uint32_t cur = cur_hunk > 0 ? cur_hunk - 1 : 0;
            for (;;) {
                uint32_t rcnt = ptr[idx++];
                uint32_t rhunk;
                uint32_t base;
                uint32_t r;
                if (!rcnt) break;
                rhunk = ptr[idx++] - (uint32_t)first_hunk;
                if (rhunk >= num_hunks) { fsreloc_last_err = 0x1B; goto fail; }
                base = (uint32_t)hunk_ptrs[rhunk];
                for (r = 0; r < rcnt; r++) {
                    uint32_t offs = ptr[idx++];
                    if (offs >= hunk_sizes[cur] * sizeof(uint32_t)) { fsreloc_last_err = 0x1C; goto fail; }
                    *(uint32_t *)((uint8_t *)hunk_ptrs[cur] + offs) += base;
                }
            }
            break;
        }
        /* HUNK_RELOC32SHORT (1020/0x3FC) and HUNK_DREL32 (1015/0x3F7, an
         * alias used by older OS37 linkers) are the compact variants:
         * all counts/hunks/offsets are 16-bit, terminator is a zero count
         * word, and the whole stream is padded to a 32-bit boundary. */
        case HUNK_RELOC32SHORT:
        case HUNK_DREL32:
        {
            uint32_t cur = cur_hunk > 0 ? cur_hunk - 1 : 0;
            uint16_t *sp = (uint16_t *)&ptr[idx];
            uint32_t consumed = 0;
            for (;;) {
                uint16_t rcnt = *sp++;
                uint16_t rhunk;
                uint32_t base;
                uint16_t r;
                consumed += 2;
                if (!rcnt) break;
                rhunk = (uint16_t)(*sp++ - (uint16_t)first_hunk);
                consumed += 2;
                if (rhunk >= num_hunks) { fsreloc_last_err = 0x1D; goto fail; }
                base = (uint32_t)hunk_ptrs[rhunk];
                for (r = 0; r < rcnt; r++) {
                    uint32_t offs = *sp++;
                    consumed += 2;
                    if (offs >= hunk_sizes[cur] * sizeof(uint32_t)) { fsreloc_last_err = 0x1E; goto fail; }
                    *(uint32_t *)((uint8_t *)hunk_ptrs[cur] + offs) += base;
                }
            }
            /* Advance idx by the number of longwords consumed (round up). */
            idx += (consumed + 3) / 4;
            break;
        }
        /* Debug / symbol / name hunks: length-prefixed in longs, we skip. */
        case HUNK_SYMBOL:
        {
            for (;;) {
                uint32_t n = ptr[idx++];
                if (!n) break;
                /* Top 8 bits are the symbol-type nibble; low 24 are the
                 * name length in longwords. Each symbol entry is name +
                 * 1 long (the value). */
                idx += (n & 0x00FFFFFFu) + 1;
            }
            break;
        }
        case HUNK_DEBUG:
        case HUNK_NAME:
            idx += ptr[idx] + 1;   /* len longs + the length word itself */
            break;
        case HUNK_END:
            if (cur_hunk >= num_hunks) {
                CacheClearU();
                return (APTR)(hunk_ptrs[0] - 1);
            }
            break;
        default:
            fsreloc_last_err = 0x20 | (htype & 0xff);
            fsreloc_last_idx = idx - 1;
            fsreloc_last_cur_hunk = cur_hunk;
            goto fail;
        }
    }

    if (cur_hunk >= num_hunks) {
        CacheClearU();
        return (APTR)(hunk_ptrs[0] - 1);
    }

    fsreloc_last_err = 0x30;
fail:
    for (h = 0; h < num_hunks && h < 16; h++) {
        if (hunk_ptrs[h]) FreeMem(hunk_ptrs[h] - 2, (hunk_sizes[h] + 2) * sizeof(uint32_t));
    }
    return NULL;
}

static struct FileSysEntry *zzsd_get_filesystem(void *boardaddr, struct sd_boot_info *info, uint32_t dostype) {
    struct FileSysResource *fsr;
    struct FileSysEntry *result_fse = NULL;
    uint32_t f;

    Forbid();
    fsr = (struct FileSysResource *)OpenResource((uint8_t *)FSRNAME);
    if (fsr) {
        struct Node *node;
        for (node = fsr->fsr_FileSysEntries.lh_Head; node->ln_Succ != NULL; node = node->ln_Succ) {
            struct FileSysEntry *fse = (struct FileSysEntry *)node;
            if (fse->fse_DosType == dostype) {
                result_fse = fse;
                break;
            }
        }
    }
    Permit();

    if (result_fse) return result_fse;

    for (f = 0; f < info->filesystem_count && f < SD_BOOT_MAX_FILESYSTEMS; f++) {
        struct sd_boot_filesystem *sf = &info->filesystems[f];
        volatile uint16_t *boot_cmd;
        volatile uint16_t *boot_status;
        uint32_t timeout;
        APTR seg;
        uint8_t *shared;
        uint8_t *local;
        uint32_t total;
        uint32_t copied;
        uint32_t chunk_idx;

        if (sf->dos_type != dostype) continue;

        debugstr(boardaddr, "FSHD match, loading...\r\n");
        boot_cmd = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_CMD);
        boot_status = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_STATUS);

        total = sf->seg_list_size;
        if (!total) continue;

        local = AllocMem(total, MEMF_PUBLIC | MEMF_CLEAR);
        if (!local) {
            debugstr(boardaddr, "FS local alloc fail.\r\n");
            return NULL;
        }

        shared = (uint8_t *)boardaddr + ZZSD_BUFFER_OFFSET;
        copied = 0;
        chunk_idx = 0;

        while (copied < total) {
            uint32_t remaining = total - copied;
            uint32_t want = remaining < ZZSD_FS_CHUNK_SIZE ? remaining : ZZSD_FS_CHUNK_SIZE;
            uint16_t cmd_word = (uint16_t)((chunk_idx << ZZSD_BOOTCMD_CHUNK_SHIFT) |
                                           ((ZZSD_BOOTCMD_LOADFS + f) & ZZSD_BOOTCMD_MASK));

            *boot_cmd = cmd_word;

            timeout = 500000;
            while (*boot_status == SD_STATUS_BUSY && timeout > 0) timeout--;
            if (timeout == 0 || *boot_status != 0) {
                debugstr(boardaddr, "FS chunk load failed.\r\n");
                FreeMem(local, total);
                return NULL;
            }

            CacheClearU();
            { volatile uint32_t *vp = (volatile uint32_t *)shared; (void)vp[0]; }
            CacheClearU();
            CopyMem(shared, local + copied, want);

            copied += want;
            chunk_idx++;
        }

        CacheClearU();
        seg = fsrelocate_simple(local, total);

        if (!seg) {
            debugstr(boardaddr, "FSreloc fail err=");
            debughex(boardaddr, fsreloc_last_err);
            debugstr(boardaddr, " idx=");
            debughex(boardaddr, fsreloc_last_idx);
            debugstr(boardaddr, " cur=");
            debughex(boardaddr, fsreloc_last_cur_hunk);
            debugstr(boardaddr, " ctx=");
            debughex(boardaddr, ((uint32_t *)local)[fsreloc_last_idx > 1 ? fsreloc_last_idx - 1 : 0]);
            debughex(boardaddr, ((uint32_t *)local)[fsreloc_last_idx]);
            debughex(boardaddr, ((uint32_t *)local)[fsreloc_last_idx + 1]);
            FreeMem(local, total);
            return NULL;
        }
        FreeMem(local, total);

        result_fse = AllocMem(sizeof(struct FileSysEntry), MEMF_PUBLIC | MEMF_CLEAR);
        if (!result_fse) return NULL;

        result_fse->fse_PatchFlags = sf->patch_flags;
        result_fse->fse_DosType = sf->dos_type;
        result_fse->fse_Version = sf->version;
        if (sf->patch_flags & FSE_TYPE) result_fse->fse_Type = sf->type;
        if (sf->patch_flags & FSE_TASK) result_fse->fse_Task = sf->task;
        if (sf->patch_flags & FSE_LOCK) result_fse->fse_Lock = sf->lock;
        if (sf->patch_flags & FSE_HANDLER) result_fse->fse_Handler = sf->handler;
        if (sf->patch_flags & FSE_STACKSIZE) result_fse->fse_StackSize = sf->stack_size;
        if (sf->patch_flags & FSE_PRIORITY) result_fse->fse_Priority = sf->priority;
        if (sf->patch_flags & FSE_STARTUP) result_fse->fse_Startup = sf->startup;
        if (sf->patch_flags & FSE_GLOBALVEC) result_fse->fse_GlobalVec = sf->global_vec;
        result_fse->fse_SegList = MKBADDR(seg);

        Forbid();
        fsr = (struct FileSysResource *)OpenResource((uint8_t *)FSRNAME);
        if (!fsr) {
            fsr = AllocMem(sizeof(struct FileSysResource), MEMF_PUBLIC | MEMF_CLEAR);
            if (fsr) {
                fsr->fsr_Node.ln_Type = NT_RESOURCE;
                my_NewList(&fsr->fsr_FileSysEntries);
                AddTail(&SysBase->ResourceList, &fsr->fsr_Node);
            }
        }
        if (fsr) AddHead(&fsr->fsr_FileSysEntries, &result_fse->fse_Node);
        Permit();

        debugstr(boardaddr, "FS registered.\r\n");
        return result_fse;
    }

    debugstr(boardaddr, "No matching FSHD.\r\n");
    return NULL;
}

static void zzsd_apply_patch_flags(struct DeviceNode *node, struct FileSysEntry *fse, void *boardaddr) {
    uint32_t pf = fse->fse_PatchFlags;
    debugstr(boardaddr, "pflags=");
    debughex(boardaddr, pf);
    if (pf & FSE_TYPE) node->dn_Type = fse->fse_Type;
    if (pf & FSE_STACKSIZE) {
        /* RKM: fhb_StackSize is in LONGWORDS; dn_StackSize in BYTES.
         * Floor at 16 KB — PFS3 under heavy random-access benchmarks
         * (SysSpeed) recurses deeper than the 0x800-longs (8 KB) that
         * stock FSHDs ship with, and a blown stack presents as illegal
         * instruction (80000004) when its frame collides with code
         * pages the OS just relocated. */
        uint32_t stk = fse->fse_StackSize * 4;
        if (stk < 16384) stk = 16384;
        node->dn_StackSize = stk;
    }
    if (pf & FSE_PRIORITY) node->dn_Priority = fse->fse_Priority;
    if (pf & FSE_SEGLIST) node->dn_SegList = fse->fse_SegList;
    if (pf & FSE_GLOBALVEC) node->dn_GlobalVec = fse->fse_GlobalVec;
}

static void zzsd_boot_process_partitions(void *boardaddr, struct sd_boot_info *info, struct ConfigDev *cd) {
    struct Library *ExpansionBase = (struct Library *)OpenLibrary((uint8_t *)"expansion.library", 0L);
    uint32_t i;
    static char execName[] = "zzsd.device";
    static char dosName[32];

    if (!ExpansionBase) return;

    for (i = 0; i < info->partition_count && i < SD_BOOT_MAX_PARTITIONS; i++) {
        struct sd_boot_partition *sp = &info->partitions[i];
        ULONG parmPkt[24];
        struct FileSysEntry *fse;
        struct DeviceNode *node;
        uint8_t *name_raw;
        uint8_t name_len;
        LONG bootPri;
        uint32_t j;

        memset(parmPkt, 0, sizeof(parmPkt));
        name_raw = (uint8_t *)sp->drive_name;
        name_len = name_raw[0];
        if (name_len > 30) name_len = 30;
        for (j = 0; j < name_len; j++) dosName[j] = name_raw[j + 1];
        dosName[name_len] = 0;

        parmPkt[0] = (ULONG)dosName;
        parmPkt[1] = (ULONG)execName;
        parmPkt[2] = 0;
        parmPkt[3] = 0;

        for (j = 0; j < 20; j++) parmPkt[4 + j] = sp->environment[j];
        if (parmPkt[4] > 16) parmPkt[4] = 16;

        fse = zzsd_get_filesystem(boardaddr, info, sp->environment[16]);
        bootPri = (sp->flags & 1) ? (LONG)sp->environment[15] : -128;

        node = MakeDosNode(parmPkt);
        if (!node) continue;

        node->dn_GlobalVec = (BPTR)(-1);
        if (fse) {
            zzsd_apply_patch_flags(node, fse, boardaddr);
            debugstr(boardaddr, "PatchFlags applied.\r\n");
        }

        debugstr(boardaddr, "stk=");
        debughex(boardaddr, node->dn_StackSize);
        debugstr(boardaddr, "seg=");
        debughex(boardaddr, (uint32_t)node->dn_SegList);
        debugstr(boardaddr, "gvec=");
        debughex(boardaddr, (uint32_t)node->dn_GlobalVec);
        debugstr(boardaddr, "stup=");
        debughex(boardaddr, (uint32_t)node->dn_Startup);
        debugstr(boardaddr, "ts=");
        debughex(boardaddr, parmPkt[4]);
        debugstr(boardaddr, "bsz=");
        debughex(boardaddr, parmPkt[5]);
        debugstr(boardaddr, "bmt=");
        debughex(boardaddr, parmPkt[4 + 12]);
        debugstr(boardaddr, "dt=");
        debughex(boardaddr, parmPkt[4 + 16]);

        if (bootPri > -128) {
            AddBootNode(bootPri, 0, node, cd);
            debugstr(boardaddr, "AddBootNode done.\r\n");
        } else {
            AddBootNode(bootPri, 0, node, NULL);
            debugstr(boardaddr, "AddBootNode(nb) done.\r\n");
        }
    }

    CloseLibrary(ExpansionBase);
}

void zzsd_boot_init(void *boardaddr, struct SDBase *sdb, struct ConfigDev *cd) {
    volatile uint16_t *boot_cmd;
    volatile uint16_t *boot_status;
    struct sd_boot_info *shared;
    struct sd_boot_info *info;
    uint32_t timeout;

    debugstr(boardaddr, "Scanning for RDB...\r\n");
    boot_cmd = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_CMD);
    boot_status = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_STATUS);

    *boot_cmd = ZZSD_BOOTCMD_GETINFO;
    timeout = 500000;
    while (*boot_status == SD_STATUS_BUSY && timeout > 0) timeout--;

    if (timeout == 0 || *boot_status != 0) {
        debugstr(boardaddr, "Boot info failed.\r\n");
        return;
    }

    CacheClearU();
    shared = (struct sd_boot_info *)((uint8_t *)boardaddr + ZZSD_BUFFER_OFFSET);
    if (shared->magic != SD_BOOT_MAGIC) {
        debugstr(boardaddr, "No boot magic.\r\n");
        return;
    }

    /* Snapshot the boot_info into local Amiga RAM. Any subsequent LOADFS
     * command overwrites the shared Zorro buffer with filesystem data,
     * so we can't read partition_count / filesystems[] from `shared`
     * once we start processing partitions. */
    info = AllocMem(sizeof(struct sd_boot_info), MEMF_PUBLIC | MEMF_CLEAR);
    if (!info) {
        debugstr(boardaddr, "boot_info alloc fail.\r\n");
        return;
    }
    CopyMem(shared, info, sizeof(struct sd_boot_info));

    sdb->sd_BaseOffset = info->rdb_start_block;
    sdb->sd_PartBlocks = info->partition_blocks;

    debugstr(boardaddr, "base=");
    debughex(boardaddr, sdb->sd_BaseOffset);
    debugstr(boardaddr, "size=");
    debughex(boardaddr, sdb->sd_PartBlocks);

    if (info->partition_count > 0) {
        debugstr(boardaddr, "RDB found!\r\n");
        zzsd_boot_process_partitions(boardaddr, info, cd);
    } else {
        debugstr(boardaddr, "No RDB, device ready.\r\n");
    }
    FreeMem(info, sizeof(struct sd_boot_info));
}
