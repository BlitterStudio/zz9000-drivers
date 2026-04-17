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

static void my_NewList(struct List *l) {
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)l;
}

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

    if (size < 20) return NULL;
    if (ptr[0] != HUNK_HEADER) return NULL;
    if (ptr[1] != 0) return NULL;

    num_hunks = ptr[2];
    if (num_hunks == 0) return NULL;

    first_hunk = (int32_t)ptr[3];
    last_hunk = (int32_t)ptr[4];
    if (first_hunk < 0 || last_hunk < 0 || first_hunk > last_hunk) return NULL;

    num_hunks = (uint32_t)(last_hunk - first_hunk + 1);
    if (num_hunks > 16) return NULL;

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
        if (!hunk) goto fail;

        hunk[0] = hsize + 2;
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
            if (cur_hunk >= num_hunks) goto fail;
            hsize = ptr[idx++];
            if (hsize > hunk_sizes[cur_hunk]) goto fail;
            CopyMem(&ptr[idx], hunk_ptrs[cur_hunk], hsize * sizeof(uint32_t));
            idx += hsize;
            cur_hunk++;
            break;
        }
        case HUNK_BSS:
            if (cur_hunk >= num_hunks) goto fail;
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
                if (rhunk >= num_hunks) goto fail;
                base = (uint32_t)hunk_ptrs[rhunk];
                for (r = 0; r < rcnt; r++) {
                    uint32_t offs = ptr[idx++];
                    if (offs >= hunk_sizes[cur] * sizeof(uint32_t)) goto fail;
                    *(uint32_t *)((uint8_t *)hunk_ptrs[cur] + offs) += base;
                }
            }
            break;
        }
        case HUNK_END:
            if (cur_hunk >= num_hunks) {
                CacheClearU();
                return (APTR)(hunk_ptrs[0] - 1);
            }
            break;
        default:
            goto fail;
        }
    }

    if (cur_hunk >= num_hunks) {
        CacheClearU();
        return (APTR)(hunk_ptrs[0] - 1);
    }

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
        uint8_t *buffer;

        if (sf->dos_type != dostype) continue;

        debugstr(boardaddr, "FSHD match, loading...\r\n");
        boot_cmd = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_CMD);
        boot_status = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_BOOT_STATUS);
        *boot_cmd = ZZSD_BOOTCMD_LOADFS + f;

        timeout = 500000;
        while (*boot_status == SD_STATUS_BUSY && timeout > 0) timeout--;
        if (timeout == 0 || *boot_status != 0) {
            debugstr(boardaddr, "FS load failed.\r\n");
            return NULL;
        }

        buffer = (uint8_t *)boardaddr + ZZSD_BUFFER_OFFSET;
        CacheClearU();
        seg = NULL;
        if (sf->seg_list_size) seg = fsrelocate_simple(buffer, sf->seg_list_size);
        if (!seg) {
            debugstr(boardaddr, "FS relocate FAILED.\r\n");
            return NULL;
        }

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
    if (pf & FSE_STACKSIZE) node->dn_StackSize = fse->fse_StackSize;
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
        ULONG *env;
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

        env = (ULONG *)&sp->size_block;
        for (j = 0; j < 14; j++) parmPkt[4 + j] = env[j];
        parmPkt[4 + 14] = sp->boot_priority;
        parmPkt[4 + 15] = sp->reserved;
        parmPkt[4 + 16] = sp->dos_type;
        parmPkt[4 + 17] = sp->reserved;
        parmPkt[4 + 18] = sp->low_cyl;
        parmPkt[4 + 19] = sp->high_cyl;

        if (parmPkt[4] > 16) parmPkt[4] = 16;

        fse = zzsd_get_filesystem(boardaddr, info, sp->dos_type);
        bootPri = (sp->flags & 1) ? (LONG)sp->boot_priority : -128;

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
    info = (struct sd_boot_info *)((uint8_t *)boardaddr + ZZSD_BUFFER_OFFSET);
    if (info->magic != SD_BOOT_MAGIC) {
        debugstr(boardaddr, "No boot magic.\r\n");
        return;
    }

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
}
