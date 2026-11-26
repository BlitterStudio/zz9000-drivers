/*
 * Test of the ZZ9000AX driver of WHDLoad.Audio (zz9000.s).
 *
 * The driver is assembled as a flat blob by tests/harness.s and runs here
 * under qemu-m68k. This program provides what the driver finds on an Amiga:
 * exec, expansion.library, dos.library and zz9k.library as stubs, the board
 * memory, and a model of the firmware's direct-ring lease: producer line
 * checks (seqlock, generation, write cursor, 2000 ms heartbeat timeout),
 * the preroll before a converting lease plays, consumption in whole 20 ms
 * source periods, and credits that trail the consumption by the fill-ahead.
 * The audio the model consumes is compared with the expected output.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;

/* offsets and entry points, in the order of h_ctl in harness.s */
enum ctl {
    C_CFN, C_EXEC, C_INIT, C_EXIT, C_PLAY, C_STOP, C_SUSPEND, C_RESUME,
    C_VOLUME, C_IRQ, C_CALLCHK, C_RUNSERVER, C_VARS, C_DRIVER, C_STUB0,
    C_STUB10 = C_STUB0 + 10,
    C_CD_ADDR, C_CD_SIZE, C_IS_DATA, C_IS_CODE, C_LN_TYPE, C_LN_NAME,
    C_LIB_VER, C_LIB_REV,
    V_ZZ9K, V_HW, V_RING, V_CTRL, V_CAP, V_GEN, V_SLOT, V_WRHI, V_WRLO,
    V_HB, V_PFLAGS, V_VOL, V_STATE, V_FINPEND, V_LIVE, V_DEAD, V_ARMED,
    V_SRV, V_MEM, V_INT, V_ST_TRACKS, V_ST_REFUSED, V_ST_IRQS, V_DEADSTAT,
    V_LEFT, V_TOTAL, V_FMT, V_SIZEOF, V_VARSIZE, V_OFF, V_PAD, V_ST_SUSP,
    V_ST_RESUME
};

enum { ID_TRACE, ID_OPENLIB, ID_CLOSELIB, ID_ADDINT, ID_REMINT, ID_CACHE,
       ID_FINDCD, ID_DOSOPEN, ID_DOSCLOSE, ID_QCAPS, ID_CALL };

#define PERIOD      3528        /* bytes of 20 ms at 44100 Hz, stereo 16 bit */
#define PREROLL     23040
#define LEAD        (12 * PERIOD)
#define CALLMAX     (3 * PERIOD)
#define RING_CAP    61440
#define BOARD_SIZE  0x400000
#define CTRL1       0x200000
#define RING1       0x200080
#define CTRL2       0x210000
#define RING2       0x210080
#define VOL_FULL    0x7fff
#define WAUDIRQF_FINISHED 1
#define WHDLAUDF_LOOP     1
#define ST_IDLE     0
#define ST_PLAY     1
#define ST_PAD      2
#define ST_FIN      3

static u8 *blob;
static u32 *ctl;
static u8 *vars;
static u8 board[BOARD_SIZE] __attribute__((aligned(4096)));

static int failures;
static int checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put32(u8 *p, u32 v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static u32 V32(int idx)
{
    return be32(vars + ctl[idx]);
}

static u8 V8(int idx)
{
    return vars[ctl[idx]];
}

static u8 *addr(u32 a)
{
    return (u8 *)(uintptr_t)a;
}

static u32 a32(const void *p)
{
    return (u32)(uintptr_t)p;
}

/* ------------------------------------------------------------------ */
/* the firmware model */

struct config {
    int have_expansion;
    int product;              /* 4 Zorro III, 3 Zorro II, 0 no card */
    int env_int2;
    int cfg_int2;             /* ZZ9000.CFG key present */
    int have_lib;
    int lib_version, lib_revision;
    u32 caps_bits;
    u32 service_flags;
    int slot_status[3];       /* status of the acquire per slot */
    int bad_grant;            /* see fw_call */
    int old_firmware;         /* no IDLE_PAUSED flag: BAD_REQUEST */
};

static struct config cfg;

#define CAP_FABRIC (1u << 27)
#define SF_FABRIC (1u << 22)
#define SF_RATE (1u << 23)

struct lease {
    int active, revoked;
    u32 slot, gen;
    u8 *ring, *ctrl;
    u32 cap;
    u64 W, consumed, credited;
    u32 hb, age, max_age;
    u64 max_lead;
    int paused, live, stall, torn, idle_paused;
    u32 underruns;
    u64 uev[256];
    u32 silent_ticks;
    u64 hist[8];
    u64 tick;
    u8 *out;
    size_t outlen, outcap;
    u32 fw_status;
    u32 hb_changes;
};

static struct lease lease[3];
static u32 next_gen = 1;
static int released[3];
static u32 released_gen[3];
static int release_calls;
static int acquire_calls;

static u8 *expbase, *dosbase, *zzbase, *execbase;
static u8 cdbuf[128];
static int opens[3], closes[3];
static int add_calls, rem_calls;   /* calls the driver must not make */
static int cache_calls;
static u32 cache_last_addr;
static char tracelog[64][400];
static int ntrace;

static void os_trace(const char *fmt, const u32 *args)
{
    char *o = tracelog[ntrace < 63 ? ntrace : 63];
    size_t n = 0;

    if (ntrace < 63)
        ntrace++;
    o[0] = 0;
    while (*fmt && n < sizeof(tracelog[0]) - 60) {
        if (*fmt != '%') {
            o[n++] = *fmt++;
            continue;
        }
        fmt++;
        if (fmt[0] == 'l' && fmt[1] == 'd') {
            n += sprintf(o + n, "%d", (int)*args++);
            fmt += 2;
        } else if (fmt[0] == 'l' && fmt[1] == 'x') {
            n += sprintf(o + n, "%x", (unsigned)*args++);
            fmt += 2;
        } else if (fmt[0] == 's') {
            n += sprintf(o + n, "%s", (const char *)(uintptr_t)*args++);
            fmt++;
        } else {
            o[n++] = '%';
        }
    }
    o[n] = 0;
}

static int traced(const char *needle)
{
    int i;

    for (i = 0; i < ntrace; i++)
        if (strstr(tracelog[i], needle))
            return 1;
    return 0;
}

static void lease_reset_output(struct lease *L)
{
    free(L->out);
    L->out = NULL;
    L->outlen = L->outcap = 0;
}

static void lease_out(struct lease *L, const u8 *p, size_t n)
{
    if (L->outlen + n > L->outcap) {
        L->outcap = (L->outlen + n) * 2 + 65536;
        L->out = realloc(L->out, L->outcap);
    }
    memcpy(L->out + L->outlen, p, n);
    L->outlen += n;
}

/* the firmware writes its line: sequence, generation, credits, status */
static void fw_publish(struct lease *L)
{
    u8 *f = L->ctrl + 64;
    u32 seq = (be32(f) + 2) & ~1u;

    put32(f, seq | 1);
    put32(f + 4, L->gen);
    put32(f + 8, (u32)(L->credited >> 32));
    put32(f + 12, (u32)L->credited);
    put32(f + 16, L->fw_status);
    put32(f, seq);
}

static void fw_revoke(struct lease *L, u32 status)
{
    L->fw_status = status;
    fw_publish(L);
    L->active = 0;
    L->revoked = (int)status;
}

/* one 20 ms tick of the compositor for a lease */
static void fw_tick(struct lease *L)
{
    u8 *c;
    u32 seq1, seq2, gen, flags, hb;
    u64 W;

    if (!L->active)
        return;
    c = L->ctrl;
    L->tick++;
    seq1 = be32(c);
    if (seq1 & 1) {
        L->age++;
    } else {
        gen = be32(c + 4);
        W = ((u64)be32(c + 8) << 32) | be32(c + 12);
        hb = be32(c + 16);
        flags = be32(c + 20);
        seq2 = be32(c);
        if (seq1 == seq2 && gen == L->gen) {
            if (W < L->W || W < L->credited || W - L->credited > L->cap) {
                fw_revoke(L, 2);
                return;
            }
            if (flags & ~1u) {
                L->age++;
            } else {
                L->W = W;
                if (W - L->credited > L->max_lead)
                    L->max_lead = W - L->credited;
                L->paused = flags & 1;
                if (L->idle_paused && L->paused) {
                    L->hb = hb;
                    L->age = 0;
                } else if (hb != L->hb) {
                    L->hb = hb;
                    L->age = 0;
                    L->hb_changes++;
                } else {
                    L->age++;
                }
            }
        } else {
            L->age++;
        }
    }
    if (L->age > L->max_age)
        L->max_age = L->age;
    if (L->idle_paused && L->paused)
        L->age = 0;           /* seqlock misses of a paused line do not age */
    if (L->age >= 100) {
        fw_revoke(L, 1);
        return;
    }
    if (!L->live && !L->paused && L->W - L->credited >= PREROLL)
        L->live = 1;
    if (L->live && !L->stall) {
        if (L->paused) {
            L->silent_ticks++;
        } else if (L->W - L->consumed >= PERIOD) {
            u32 off = (u32)(L->consumed % L->cap);
            u32 first = L->cap - off;

            if (first > PERIOD)
                first = PERIOD;
            lease_out(L, L->ring + off, first);
            if (first < PERIOD)
                lease_out(L, L->ring, PERIOD - first);
            L->consumed += PERIOD;
        } else {
            if (L->underruns < 256)
                L->uev[L->underruns] = L->consumed;
            L->underruns++;
        }
    }
    L->hist[L->tick & 7] = L->consumed;
    {
        u64 credited = L->tick >= 6 ? L->hist[(L->tick - 6) & 7] : 0;

        if (credited != L->credited) {
            L->credited = credited;
            fw_publish(L);
        }
    }
    if (L->torn)
        put32(L->ctrl + 64, be32(L->ctrl + 64) | 1);   /* stuck in an update */
}

static void fw_tick_all(void)
{
    fw_tick(&lease[1]);
    fw_tick(&lease[2]);
}

/* the mailbox */
static u32 fw_call(u8 *req, u8 *rep)
{
    u16 op = (req[4] << 8) | req[5];
    u8 *pl = req + 16;
    u8 *rp = rep + 16;

    memset(rep, 0, 64);
    CHECK(((req[8] << 8) | req[9]) == 1, "inline flag");
    CHECK(((req[10] << 8) | req[11]) == 48, "payload length");
    if (op == 0x0004) {
        CHECK(be32(pl) == 0x0500, "service id");
        put32(rp + 12, cfg.service_flags);
        return 0;
    }
    if (op == 0x0513) {
        u32 slot = be32(pl);
        struct lease *L;
        u32 ring_off, ctrl_off, cap = RING_CAP;

        acquire_calls++;
        CHECK(be32(pl + 4) == 2, "identity %u", be32(pl + 4));
        CHECK(be32(pl + 8) == 128, "gain %u", be32(pl + 8));
        CHECK(be32(pl + 12) == 5, "acquire flags %u", be32(pl + 12));
        if (cfg.old_firmware)
            return 4;
        CHECK(be32(pl + 16) == 44100, "rate %u", be32(pl + 16));
        if (slot < 1 || slot > 2)
            return 4;
        if (cfg.slot_status[slot])
            return (u32)cfg.slot_status[slot];
        L = &lease[slot];
        ring_off = slot == 1 ? RING1 : RING2;
        ctrl_off = slot == 1 ? CTRL1 : CTRL2;
        lease_reset_output(L);
        memset(L, 0, sizeof(*L));
        L->active = 1;
        L->slot = slot;
        L->gen = next_gen++;
        L->ring = board + ring_off;
        L->ctrl = board + ctrl_off;
        L->cap = cap;
        L->idle_paused = (be32(pl + 12) & 4) != 0;
        memset(L->ring, 0, cap);
        memset(L->ctrl, 0, 128);
        fw_publish(L);
        put32(rp, slot);
        put32(rp + 4, L->gen);
        put32(rp + 8, ring_off);
        put32(rp + 12, cap);
        put32(rp + 16, ctrl_off);
        put32(rp + 20, 3840);
        put32(rp + 24, 20000);
        put32(rp + 28, 2);
        put32(rp + 32, 128);
        put32(rp + 36, 2);
        put32(rp + 40, 0);
        put32(rp + 44, 44100);
        switch (cfg.bad_grant) {
        case 1: put32(rp + 4, 0); break;                    /* generation 0 */
        case 2: put32(rp + 28, 1); break;                   /* contract */
        case 3: put32(rp + 44, 48000); break;               /* rate */
        case 4: put32(rp + 12, 3840 * 4); break;            /* capacity */
        case 5: put32(rp + 16, ctrl_off + 4); break;        /* control align */
        case 6: put32(rp + 8, BOARD_SIZE - 3840); break;    /* outside */
        case 7: put32(rp + 16, ring_off + 3840); break;     /* overlap */
        case 8: put32(rp + 12, RING_CAP - 4); break;        /* not whole periods */
        case 9: put32(rp + 8, ring_off + 2); break;         /* ring align */
        case 10: put32(rp + 20, 3840 * 2); break;           /* period */
        default: break;
        }
        return 0;
    }
    if (op == 0x0514) {
        u32 slot = be32(pl), gen = be32(pl + 4);

        release_calls++;
        CHECK(be32(pl + 8) == 0, "release flags");
        if (slot >= 1 && slot <= 2) {
            released[slot]++;
            released_gen[slot] = gen;
            if (lease[slot].active && lease[slot].gen == gen)
                lease[slot].active = 0;
        }
        return 0;
    }
    CHECK(0, "unknown opcode %04x", op);
    return 4;
}

/* ------------------------------------------------------------------ */
/* the system around the driver */

static void set_lib(u8 *base, int ver, int rev)
{
    base[ctl[C_LIB_VER]] = ver >> 8;
    base[ctl[C_LIB_VER] + 1] = ver;
    base[ctl[C_LIB_REV]] = rev >> 8;
    base[ctl[C_LIB_REV] + 1] = rev;
}

static int os_back;                   /* WHDLoad has given the OS back */
static int hw_enabled;                /* what the last write to the register said */
static u16 hw16(void);

static void os_dispatch(u32 id, u32 *regs)
{
    /* With the OS back, its PORTS chain does not know the vblank interrupt
     * of the card; if the card still raises it the machine hangs in the
     * interrupt. Before the driver does anything that needs the OS, the
     * card interrupt must be off. */
    if (os_back && id != ID_TRACE)
        CHECK(hw16() == 0x80,
              "OS call %u with the card interrupt enabled (word %04x)", id,
              hw16());
    switch (id) {
    case ID_TRACE:
        os_trace((const char *)(uintptr_t)regs[2],
                 (const u32 *)(uintptr_t)regs[3]);
        break;
    case ID_OPENLIB: {
        const char *name = (const char *)(uintptr_t)regs[3];

        if (!strcmp(name, "expansion.library")) {
            regs[0] = 0;
            if (cfg.have_expansion) {
                opens[0]++;
                regs[0] = a32(expbase);
            }
        } else if (!strcmp(name, "dos.library")) {
            opens[1]++;
            regs[0] = a32(dosbase);
        } else if (!strcmp(name, "zz9k.library")) {
            CHECK(regs[0] == 2, "OpenLibrary version %u", regs[0]);
            regs[0] = 0;
            if (cfg.have_lib && cfg.lib_version >= 2) {
                opens[2]++;
                set_lib(zzbase, cfg.lib_version, cfg.lib_revision);
                regs[0] = a32(zzbase);
            }
        } else {
            CHECK(0, "unexpected library %s", name);
            regs[0] = 0;
        }
        break;
    }
    case ID_CLOSELIB:
        if (regs[3] == a32(expbase))
            closes[0]++;
        else if (regs[3] == a32(dosbase))
            closes[1]++;
        else if (regs[3] == a32(zzbase))
            closes[2]++;
        else
            CHECK(0, "CloseLibrary of %08x", regs[3]);
        break;
    case ID_ADDINT:
        CHECK(regs[0] == 5, "interrupt number %u", regs[0]);
        add_calls++;
        CHECK(0, "the driver added an interrupt server: nothing of the plugin "
                 "may stay in the operating system behind waud_init");
        break;
    case ID_REMINT:
        CHECK(regs[0] == 5, "interrupt number %u", regs[0]);
        rem_calls++;
        CHECK(0, "the driver removed an interrupt server");
        break;
    case ID_CACHE:
        cache_calls++;
        cache_last_addr = regs[2];
        CHECK(regs[1] == 0x0800, "cache flags %x", regs[1]);
        CHECK(regs[0] == 64, "cache length %u", regs[0]);
        break;
    case ID_FINDCD: {
        u32 manuf = regs[0], product = regs[1];

        CHECK(manuf == 0x6d6e, "manufacturer %x", manuf);
        CHECK(regs[2] == 0, "FindConfigDev starts from the beginning");
        regs[0] = 0;
        if (cfg.product && product == (u32)cfg.product) {
            put32(cdbuf + ctl[C_CD_ADDR], a32(board));
            put32(cdbuf + ctl[C_CD_SIZE], BOARD_SIZE);
            regs[0] = a32(cdbuf);
        }
        break;
    }
    case ID_DOSOPEN:
        regs[0] = (cfg.env_int2 &&
                   !strcmp((const char *)(uintptr_t)regs[1], "ENV:ZZ9K_INT2"))
                      ? 1 : 0;
        break;
    case ID_DOSCLOSE:
        break;
    case ID_QCAPS: {
        u8 *caps = addr(regs[2]);

        memset(caps, 0, 64);
        put32(caps + 8, cfg.caps_bits);
        regs[0] = 0;
        break;
    }
    case ID_CALL:
        regs[0] = fw_call(addr(regs[2]), addr(regs[3]));
        break;
    default:
        CHECK(0, "stub %u", id);
    }
}

/* ------------------------------------------------------------------ */
/* calling the driver */

/* WHDLoad maps the memory a plugin declares in waud_mem and nothing else of
 * the card (page tables of the 68040/060, 4 KiB pages). With the operating
 * system off every access of the driver to the board outside those pages is
 * an access fault there, so while a call of the driver runs the pages of the
 * board that are not in the list are made inaccessible here. */
static int map_board;

static void board_map(int on)
{
    u32 *m = (u32 *)(vars + ctl[V_MEM]);

    mprotect(board, BOARD_SIZE, on ? PROT_NONE : PROT_READ | PROT_WRITE);
    for (; on && be32((u8 *)m) != 0; m += 3) {
        u32 lo = be32((u8 *)m) & ~4095u;
        u32 hi = (be32((u8 *)(m + 1)) | 4095u) + 1;

        if (mprotect(addr(lo), hi - lo, PROT_READ | PROT_WRITE)) {
            perror("mprotect");
            exit(2);
        }
    }
}

typedef u32 (*chk_t)(void *fn, u32 d0, u32 d1, void *a0, u32 *bad);

/* call an entry of the driver with sentinels in the registers it must keep */
static u32 call(int idx, u32 d0, u32 d1, const void *a0)
{
    u32 bad = 0, r;
    chk_t chk = (chk_t)(uintptr_t)(a32(blob) + ctl[C_CALLCHK]);

    if (map_board && idx != C_INIT && idx != C_EXIT)
        board_map(1);
    r = chk((void *)(uintptr_t)(a32(blob) + ctl[idx]), d0, d1, (void *)a0,
            &bad);
    if (map_board && idx != C_INIT && idx != C_EXIT)
        board_map(0);
    CHECK(bad == 0, "entry %d changed registers, mask %x", idx, bad);
    return r;
}

static void config_default(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.have_expansion = 1;
    cfg.product = 4;
    cfg.env_int2 = 1;
    cfg.have_lib = 1;
    cfg.lib_version = 2;
    cfg.lib_revision = 44;
    cfg.caps_bits = CAP_FABRIC;
    cfg.service_flags = SF_FABRIC | SF_RATE;
}

static void sim_reset(void)
{
    int i;

    for (i = 1; i <= 2; i++) {
        lease_reset_output(&lease[i]);
        memset(&lease[i], 0, sizeof(lease[i]));
        released[i] = 0;
        released_gen[i] = 0;
    }
    memset(board, 0, 0x1000);
    memset(opens, 0, sizeof(opens));
    memset(closes, 0, sizeof(closes));
    add_calls = rem_calls = cache_calls = 0;
    release_calls = acquire_calls = 0;
    ntrace = 0;
    memset(vars, 0, ctl[V_VARSIZE]);
}

/* the jump table of a library is code, it needs executable memory */
static u8 *make_lib(void)
{
    u8 *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (m == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    return m + 2048;
}

static void slot_jmp(u8 *base, int lvo, int stub)
{
    u8 *p = base + lvo;

    p[0] = 0x4e;
    p[1] = 0xf9;
    put32(p + 2, a32(blob) + ctl[C_STUB0 + stub]);
}

static void world_setup(void)
{
    execbase = make_lib();
    expbase = make_lib();
    dosbase = make_lib();
    zzbase = make_lib();
    slot_jmp(execbase, -552, ID_OPENLIB);
    slot_jmp(execbase, -414, ID_CLOSELIB);
    slot_jmp(execbase, -168, ID_ADDINT);
    slot_jmp(execbase, -174, ID_REMINT);
    slot_jmp(execbase, -642, ID_CACHE);
    slot_jmp(expbase, -72, ID_FINDCD);
    slot_jmp(dosbase, -30, ID_DOSOPEN);
    slot_jmp(dosbase, -36, ID_DOSCLOSE);
    slot_jmp(zzbase, -30, ID_QCAPS);
    slot_jmp(zzbase, -48, ID_CALL);
    ctl[C_EXEC] = a32(execbase);
    ctl[C_CFN] = a32(os_dispatch);
}

/* ------------------------------------------------------------------ */
/* time: the video interrupt of the card and the compositor tick */

static int t_ms;
static int irq_period = 20;           /* 50 Hz, 17 is about 60 Hz */
static int finished_count;
static int irq_count;
static int os_active;                 /* operating system active: no irq */
static int irq_ack_errors;


static u16 hw16(void)
{
    return (board[4] << 8) | board[5];
}

static void set_hw16(u16 v)
{
    board[4] = v >> 8;
    board[5] = v;
}

static void note_hw(void)
{
    u16 v = hw16();

    if (v == 0x82)
        hw_enabled = 1;
    else if (v == 0x80)
        hw_enabled = 0;
}

static void tick_ms(void)
{
    t_ms++;
    if (!os_active && hw_enabled && t_ms % irq_period == 0) {
        u32 r;

        set_hw16(0x0004);
        r = call(C_IRQ, 0, 0, NULL);
        irq_count++;
        if (hw16() != 0x48 && hw16() != 0x80)   /* 0x80: nothing left, disabled */
            irq_ack_errors++;
        note_hw();
        if (r & WAUDIRQF_FINISHED)
            finished_count++;
        CHECK((r & ~1u) == 0, "irq result %x", r);
    }
    if (t_ms % 20 == 0) {
        fw_tick_all();
    }
}

static void run_ms(int ms)
{
    while (ms-- > 0)
        tick_ms();
}

/* ------------------------------------------------------------------ */
/* the driver */

static u32 do_init(void)
{
    u32 r;

    map_board = 0;
    sim_reset();
    board[0xea] = 0;
    board[0xeb] = cfg.cfg_int2 ? 1 : 0;
    r = call(C_INIT, 0, 0, NULL);
    note_hw();
    map_board = r == 0;
    return r;
}

static struct lease *L1(void)
{
    return &lease[V32(V_SLOT)];
}

static int fresh(void)
{
    config_default();
    t_ms = 0;
    os_back = 0;
    irq_period = 20;
    hw_enabled = 0;
    finished_count = irq_count = irq_ack_errors = 0;
    os_active = 0;
    if (do_init() != 0) {
        CHECK(0, "init failed");
        return 0;
    }
    return 1;
}

static void drv_play(const u8 *data, u32 len, u32 flags)
{
    call(C_PLAY, len, flags, data);
    note_hw();
}

static void drv_stop(void)
{
    call(C_STOP, 0, 0, NULL);
    note_hw();
}

static void drv_suspend(void)
{
    call(C_SUSPEND, 0, 0, NULL);
    note_hw();
}

static void drv_resume(void)
{
    call(C_RESUME, 0, 0, NULL);
    note_hw();
}

static void drv_volume(u32 v)
{
    call(C_VOLUME, v, 0, NULL);
}

static void drv_exit(void)
{
    os_back = 1;
    call(C_EXIT, 0, 0, NULL);
    note_hw();
    os_back = 0;
}

/* ------------------------------------------------------------------ */
/* WAV files and the expected output */

struct wav {
    u8 *data;
    u32 len;
    int ch, bits;
    u32 frames;
    const u8 *pcm;            /* the samples inside data */
};

static u32 lcg_state = 12345;

static u32 lcg(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state >> 8;
}

static void le16(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void le32(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

struct wavopt {
    int ch, bits, rate, tag;
    u32 frames;
    int list_bytes;           /* LIST chunk before data with this body size */
    int data_size_mode;       /* 0 exact, 1 size 0xffffffff, 2 size too big */
    int fmt_after_data;
    int skip_fmt;
    int trailing_chunk;
    int truncate;             /* bytes cut off the end */
    int blockalign;           /* 0 = correct */
};

static void wavopt_default(struct wavopt *o, int ch, int bits, u32 frames)
{
    memset(o, 0, sizeof(*o));
    o->ch = ch;
    o->bits = bits;
    o->rate = 44100;
    o->tag = 1;
    o->frames = frames;
}

static void put_fmt(u8 *p, const struct wavopt *o, u32 fsz)
{
    memcpy(p, "fmt ", 4);
    le32(p + 4, 16);
    le16(p + 8, o->tag);
    le16(p + 10, o->ch);
    le32(p + 12, o->rate);
    le32(p + 16, o->rate * fsz);
    le16(p + 20, o->blockalign ? (u32)o->blockalign : fsz);
    le16(p + 22, o->bits);
}

static struct wav make_wav(const struct wavopt *o)
{
    struct wav w;
    u32 fsz = (u32)o->ch * o->bits / 8;
    u32 dlen = o->frames * fsz;
    u32 list = o->list_bytes ? 8 + o->list_bytes + (o->list_bytes & 1) : 0;
    u32 tot = 12 + 24 + list + 8 + dlen + (dlen & 1) + 24 + 12;
    u8 *f = aligned_alloc(4, (tot + 16 + 3) & ~3u);
    u8 *p;
    u32 i;

    memset(f, 0, tot + 16);
    memcpy(f, "RIFF", 4);
    le32(f + 4, tot - 8);
    memcpy(f + 8, "WAVE", 4);
    p = f + 12;
    if (!o->skip_fmt && !o->fmt_after_data) {
        put_fmt(p, o, fsz);
        p += 24;
    }
    if (o->list_bytes) {
        memcpy(p, "LIST", 4);
        le32(p + 4, o->list_bytes);
        p += list;
    }
    memcpy(p, "data", 4);
    le32(p + 4, o->data_size_mode == 1 ? 0xffffffffu
               : o->data_size_mode == 2 ? dlen + 1000 : dlen);
    p += 8;
    w.pcm = p;
    for (i = 0; i < dlen; i++)
        p[i] = lcg();
    p += dlen + (dlen & 1);
    if (o->fmt_after_data) {
        put_fmt(p, o, fsz);
        p += 24;
    }
    if (o->trailing_chunk) {
        memcpy(p, "LIST", 4);
        le32(p + 4, 4);
        p += 12;
    }
    w.data = f;
    w.len = (u32)(p - f) - (u32)o->truncate;
    w.ch = o->ch;
    w.bits = o->bits;
    w.frames = o->frames;
    return w;
}

static struct wav simple_wav(int ch, int bits, u32 frames)
{
    struct wavopt o;

    wavopt_default(&o, ch, bits, frames);
    return make_wav(&o);
}

/* the converted output for volume vol, stereo S16LE */
static u8 *expected(const struct wav *w, u32 vol, size_t from_frame)
{
    u32 fsz = (u32)w->ch * w->bits / 8;
    u8 *e = malloc(w->frames * 4 + 4);
    u32 i;

    for (i = 0; i < w->frames; i++) {
        const u8 *s = w->pcm + i * fsz;
        int c;

        for (c = 0; c < 2; c++) {
            int cc = w->ch == 1 ? 0 : c;
            s32 native, r;
            s16 o;

            if (w->bits == 16)
                native = (s16)(s[cc * 2] | (s[cc * 2 + 1] << 8));
            else
                native = (s32)(s8)(s[cc] ^ 0x80) * 256;
            if (w->bits == 16 && w->ch == 2 && vol >= VOL_FULL) {
                r = native;
            } else {
                s32 p = native * (s32)vol;

                r = (s16)(((s32)((u32)p * 2u)) >> 16);
            }
            o = (s16)r;
            e[i * 4 + c * 2] = (u8)o;
            e[i * 4 + c * 2 + 1] = (u8)((u16)o >> 8);
        }
    }
    (void)from_frame;
    return e;
}

/* ------------------------------------------------------------------ */
/* scenarios */

#define MAXTIME 60000

static void run_until_finished(int max_ms)
{
    int base = finished_count;
    int t = 0;

    while (finished_count == base && t < max_ms) {
        tick_ms();
        t++;
    }
    CHECK(finished_count == base + 1, "track did not finish in %d ms", max_ms);
}

/* the played track is complete in the output of the firmware model */
static void check_stream(const char *what, const u8 *exp, size_t explen,
                         size_t at, const struct lease *L)
{
    size_t i;

    CHECK(L->outlen >= at + explen, "%s: output has %zu bytes, need %zu", what,
          L->outlen, at + explen);
    if (L->outlen >= at + explen) {
        for (i = 0; i < explen; i++)
            if (L->out[at + i] != exp[i])
                break;
        CHECK(i == explen, "%s: output differs at byte %zu (%02x, expected %02x)",
              what, i, i < explen ? L->out[at + i] : 0, i < explen ? exp[i] : 0);
    }
}

static void check_zero(const char *what, size_t from, size_t to,
                       const struct lease *L)
{
    size_t i;

    for (i = from; i < to && i < L->outlen; i++)
        if (L->out[i])
            break;
    CHECK(i >= to || i >= L->outlen, "%s: silence expected at byte %zu", what, i);
}

static void check_no_underrun(const char *what, const struct lease *L,
                              u64 end_W)
{
    u32 i;
    u32 n = L->underruns < 256 ? L->underruns : 256;

    for (i = 0; i < n; i++)
        CHECK(L->uev[i] >= end_W, "%s: starved at %llu of %llu", what,
              (unsigned long long)L->uev[i], (unsigned long long)end_W);
}

static void play_and_check(const char *what, const struct wav *w, u32 vol,
                           u32 flags, int period_ms)
{
    struct lease *L;
    u8 *exp;
    size_t padded;

    if (!fresh())
        return;
    irq_period = period_ms;
    drv_volume(vol);
    drv_play(w->data, w->len, flags);
    CHECK(hw_enabled, "%s: the card interrupt was not enabled", what);
    L = L1();
    run_until_finished(MAXTIME);
    run_ms(1500);                         /* the card plays the rest */
    exp = expected(w, vol, 0);
    padded = (size_t)w->frames * 4;
    check_stream(what, exp, padded, 0, L);
    CHECK(L->outlen % PERIOD == 0, "%s: %zu bytes, not whole periods", what,
          L->outlen);
    CHECK(L->outlen >= padded && L->outlen - padded < PERIOD + PREROLL + PERIOD,
          "%s: pad of %zu bytes", what, L->outlen - padded);
    check_zero(what, padded, L->outlen, L);
    CHECK(L->revoked == 0, "%s: the lease was revoked (%d)", what, L->revoked);
    CHECK(L->max_age < 5, "%s: heartbeat age %u ticks", what, L->max_age);
    CHECK(L->max_lead <= LEAD, "%s: lead %llu", what,
          (unsigned long long)L->max_lead);
    CHECK(L->W == ((u64)V32(V_WRHI) << 32 | V32(V_WRLO)), "%s: write cursor",
          what);
    check_no_underrun(what, L, L->W);
    CHECK(V32(V_ST_TRACKS) == 1 && V32(V_ST_REFUSED) == 0, "%s: statistics",
          what);
    CHECK(irq_ack_errors == 0, "%s: interrupt not acknowledged", what);
    CHECK(finished_count == 1, "%s: finished %d times", what, finished_count);
    CHECK(V8(V_STATE) == ST_IDLE && be32(L->ctrl + 20) == 1,
          "%s: not paused after the card played everything (state %u)", what,
          V8(V_STATE));
    CHECK(!hw_enabled && hw16() == 0x80 && V8(V_ARMED) == 0,
          "%s: the card interrupt is still enabled when nothing is left", what);
    free(exp);
}

static void test_init(void)
{
    int i;
    u8 *vp;
    u32 *tab;

    /* refusals, all without an error and with the reason in the trace */
    config_default();
    cfg.have_expansion = 0;
    CHECK(do_init() == 1, "no expansion.library");
    CHECK(traced("expansion.library"), "trace %s", tracelog[0]);

    config_default();
    cfg.product = 0;
    CHECK(do_init() == 1, "no card");
    CHECK(traced("no ZZ9000 found"), "trace");
    CHECK(opens[0] == closes[0], "expansion.library left open");

    config_default();
    cfg.product = 3;
    CHECK(do_init() == 1, "Zorro II");
    CHECK(traced("Zorro II"), "trace");
    CHECK(opens[2] == 0, "zz9k.library opened for a Zorro II card");

    config_default();
    cfg.env_int2 = 0;
    cfg.cfg_int2 = 0;
    CHECK(do_init() == 1, "no INT2");
    CHECK(traced("INT2 mode"), "trace");
    CHECK(opens[1] == closes[1], "dos.library left open");

    config_default();
    cfg.env_int2 = 0;
    cfg.cfg_int2 = 1;
    CHECK(do_init() == 0, "int2 = on in ZZ9000.CFG");
    CHECK(closes[1] == opens[1], "dos.library left open");

    config_default();
    cfg.have_lib = 0;
    CHECK(do_init() == 1, "no zz9k.library");
    CHECK(traced("zz9k.library"), "trace");

    config_default();
    cfg.lib_revision = 43;
    CHECK(do_init() == 1, "zz9k.library 2.43");
    CHECK(traced("too old") && traced("2.43"), "trace %s", tracelog[0]);
    CHECK(opens[2] == 1 && closes[2] == 1, "zz9k.library left open");

    config_default();
    cfg.lib_version = 3;
    cfg.lib_revision = 0;
    CHECK(do_init() == 0, "zz9k.library 3.0");

    config_default();
    cfg.caps_bits = 0;
    CHECK(do_init() == 1, "no fabric capability");
    CHECK(traced("audio fabric") && closes[2] == 1, "trace");

    config_default();
    cfg.service_flags = SF_RATE;
    CHECK(do_init() == 1, "no fabric flag");
    CHECK(closes[2] == 1 && acquire_calls == 0, "cleanup");

    config_default();
    cfg.service_flags = SF_FABRIC;
    CHECK(do_init() == 1, "no rate flag");
    CHECK(traced("source rate") && acquire_calls == 0, "trace");

    config_default();
    cfg.old_firmware = 1;
    CHECK(do_init() == 1, "firmware without IDLE_PAUSED");
    CHECK(traced("IDLE_PAUSED") && closes[2] == 1 && V8(V_LIVE) == 0,
          "trace %s", tracelog[0]);

    config_default();
    cfg.slot_status[1] = 2;
    CHECK(do_init() == 0, "slot 2 after slot 1 busy");
    CHECK(V32(V_SLOT) == 2 && acquire_calls == 2, "slot %u", V32(V_SLOT));

    config_default();
    cfg.slot_status[1] = 2;
    cfg.slot_status[2] = 2;
    CHECK(do_init() == 1, "both slots busy");
    CHECK(traced("no free direct ring lease, status 2") && closes[2] == 1,
          "trace %s", tracelog[0]);
    CHECK(V8(V_LIVE) == 0, "state");

    for (i = 1; i <= 10; i++) {
        config_default();
        cfg.bad_grant = i;
        CHECK(do_init() == 1, "bad grant %d accepted", i);
        CHECK(traced("unusable lease"), "bad grant %d: trace", i);
        CHECK(release_calls == 1 && released[1] == 1 &&
              (i == 1 || released_gen[1] == lease[1].gen),
              "bad grant %d: lease not released", i);
        CHECK(closes[2] == 1 && V8(V_LIVE) == 0,
              "bad grant %d: cleanup", i);
    }

    /* success */
    config_default();
    CHECK(do_init() == 0, "init");
    CHECK(V8(V_LIVE) != 0 && V32(V_SLOT) == 1 && V32(V_GEN) == lease[1].gen,
          "lease variables");
    CHECK(V32(V_HW) == a32(board), "board address");
    CHECK(V32(V_CAP) == RING_CAP, "capacity");
    CHECK(V32(V_RING) == a32(board) + RING1 && V32(V_CTRL) == a32(board) + CTRL1,
          "ring and control addresses");
    CHECK(V32(V_VOL) == VOL_FULL, "volume");
    CHECK(opens[2] == 1 && closes[2] == 0, "zz9k.library stays open");
    CHECK(opens[0] == closes[0] && opens[1] == closes[1], "libraries");
    CHECK(traced("lease slot 1 generation") && traced("ring capacity 61440"), "trace %s",
          tracelog[ntrace - 1]);

    /* memory list for WHDLoad */
    vp = vars + ctl[V_MEM];
    CHECK(be32(vp) == a32(board) && be32(vp + 4) == a32(board) + 0xfff &&
          be32(vp + 8) == 2, "registers in the memory list");
    CHECK(be32(vp + 12) == a32(board) + RING1 &&
          be32(vp + 16) == a32(board) + RING1 + RING_CAP - 1 &&
          be32(vp + 20) == 2, "ring in the memory list");
    CHECK(be32(vp + 24) == a32(board) + CTRL1 &&
          be32(vp + 28) == a32(board) + CTRL1 + 127 && be32(vp + 32) == 2,
          "control block in the memory list");
    CHECK(be32(vp + 36) == 0, "memory list terminator");

    /* driver table: name, init, exts, mem, exit, play, stop, suspend, resume,
     * volume, irq; the blob has no relocations, so it holds offsets */
    tab = (u32 *)(blob + ctl[C_DRIVER]);
    CHECK(tab[1] == ctl[C_INIT] && tab[4] == ctl[C_EXIT] &&
          tab[5] == ctl[C_PLAY] && tab[6] == ctl[C_STOP] &&
          tab[7] == ctl[C_SUSPEND] && tab[8] == ctl[C_RESUME] &&
          tab[9] == ctl[C_VOLUME] && tab[10] == ctl[C_IRQ],
          "driver table order");
    CHECK(tab[3] == ctl[C_VARS] + ctl[V_MEM], "memory list pointer");
    CHECK(!strcmp((char *)blob + tab[2], "wav") && blob[tab[2] + 4] == 0,
          "extensions");
    CHECK(!strcmp((char *)blob + tab[0], "ZZ9000AX"), "name");
    CHECK(ctl[V_SIZEOF] <= ctl[V_VARSIZE], "variables %u of %u bytes",
          ctl[V_SIZEOF], ctl[V_VARSIZE]);

    /* no hook in the operating system, and the first publication */
    CHECK(add_calls == 0 && rem_calls == 0, "interrupt servers");
    vp = lease[1].ctrl;
    CHECK((be32(vp) & 1) == 0 && be32(vp) != 0, "producer sequence %u", be32(vp));
    CHECK(be32(vp + 4) == lease[1].gen && be32(vp + 8) == 0 && be32(vp + 12) == 0,
          "producer line generation and cursor");
    CHECK(be32(vp + 16) != 0, "heartbeat");
    CHECK(be32(vp + 20) == 1, "producer line paused");
    CHECK(cache_calls >= 1 && cache_last_addr == a32(lease[1].ctrl),
          "control line flushed");
    CHECK(hw16() == 0, "init must not touch the interrupt register");
    drv_exit();
    CHECK(add_calls == 0 && rem_calls == 0, "interrupt servers after exit");
    CHECK(released[1] == 1 && released_gen[1] == lease[1].gen, "lease released");
    CHECK(closes[2] == 1, "zz9k.library closed");
    CHECK(traced("intact"), "exit trace");
}

static void test_play(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 3);

    play_and_check("stereo16 3 s", &w, VOL_FULL, 0, 20);
    play_and_check("stereo16 at 60 Hz", &w, VOL_FULL, 0, 17);
    play_and_check("stereo16 at 20 Hz", &w, VOL_FULL, 0, 50);
    play_and_check("stereo16 at 75 Hz", &w, VOL_FULL, 0, 13);
    play_and_check("stereo16 at 50 Hz, volume 0x4000", &w, 0x4000, 0, 20);
    free(w.data);
}

static void test_formats(void)
{
    static const u32 vols[] = { VOL_FULL, 0x4000, 0x1234, 1, 0 };
    int ch, bits, v, i;
    char name[64];

    for (bits = 8; bits <= 16; bits += 8)
        for (ch = 1; ch <= 2; ch++)
            for (v = 0; v < 5; v++) {
                struct wav w = simple_wav(ch, bits, 17640 + (u32)v * 13);

                snprintf(name, sizeof(name), "%d bit, %d channel, volume %04x",
                         bits, ch, vols[v]);
                play_and_check(name, &w, vols[v], 0, 20);
                free(w.data);
            }

    /* stereo 16 bit at full volume with the samples word aligned only */
    for (i = 0; i < 2; i++) {
        struct wavopt o;
        struct wav w;

        wavopt_default(&o, 2, 16, 30000);
        o.list_bytes = i ? 0 : 2;     /* the samples at 54 or at 44 */
        w = make_wav(&o);
        CHECK(((u32)(uintptr_t)w.pcm & 3) == (i ? 0u : 2u), "alignment %p", w.pcm);
        play_and_check(i ? "long aligned samples" : "word aligned samples", &w,
                       VOL_FULL, 0, 20);
        free(w.data);
    }
}

/* the volume changes in the middle of a track */
static void test_volume_change(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 2);
    u8 *e_full, *e_half;
    u32 frame;
    struct lease *L;
    size_t i;

    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(500);
    frame = V32(V_TOTAL) - V32(V_LEFT);
    CHECK(frame > 0 && frame < w.frames, "frames written %u", frame);
    drv_volume(0x2000);
    run_until_finished(MAXTIME);
    run_ms(1500);
    e_full = expected(&w, VOL_FULL, 0);
    e_half = expected(&w, 0x2000, 0);
    check_stream("before the change", e_full, (size_t)frame * 4, 0, L);
    for (i = (size_t)frame * 4; i < (size_t)w.frames * 4; i++)
        if (L->out[i] != e_half[i])
            break;
    CHECK(i == (size_t)w.frames * 4, "after the change differs at %zu (frame %u)",
          i, frame);
    free(e_full);
    free(e_half);
    free(w.data);

    /* volume is limited to $7fff */
    drv_volume(0xffffffffu);
    CHECK(V32(V_VOL) == VOL_FULL, "volume limit %x", V32(V_VOL));
}

static void test_loop_stop(void)
{
    struct wav w = simple_wav(2, 16, 13230);          /* 0.3 s */
    u8 *e;
    struct lease *L;
    size_t i, n, w_stop;
    u32 flags_line;

    if (!fresh())
        return;
    drv_play(w.data, w.len, WHDLAUDF_LOOP);
    L = L1();
    run_ms(3000);
    CHECK(finished_count == 0, "a looping track must not finish");
    e = expected(&w, VOL_FULL, 0);
    n = L->outlen;
    CHECK(n >= 100000, "loop output %zu", n);
    for (i = 0; i < n; i++)
        if (L->out[i] != e[i % (w.frames * 4)])
            break;
    CHECK(i == n, "loop differs at %zu", i);
    CHECK(V8(V_STATE) == ST_PLAY, "state %u", V8(V_STATE));

    /* stop: no end of track, the lease is paused, nothing more is written */
    drv_stop();
    w_stop = V32(V_WRLO);
    flags_line = be32(L->ctrl + 20);
    CHECK(flags_line == 1, "paused after stop, flags %u", flags_line);
    CHECK(V8(V_STATE) == ST_IDLE && V8(V_FINPEND) == 0, "stop state");
    CHECK(!hw_enabled && hw16() == 0x80, "stop disables the card interrupt");
    run_ms(2000);
    CHECK(V32(V_WRLO) == w_stop, "written after stop");
    CHECK(finished_count == 0, "stop must not report the end");
    CHECK(L->silent_ticks > 50, "paused ticks %u", L->silent_ticks);
    CHECK(L->revoked == 0 && L->max_age < 5, "heartbeat while stopped, age %u",
          L->max_age);
    free(e);
    free(w.data);
}

static void test_sequence_and_replace(void)
{
    struct wav a = simple_wav(2, 16, 44100 + 500);
    struct wav b = simple_wav(1, 8, 30000);
    struct lease *L;
    u8 *ea, *eb;
    size_t pad_a, w_stop;

    /* the next track is started when the first one has finished */
    if (!fresh())
        return;
    drv_play(a.data, a.len, 0);
    L = L1();
    run_until_finished(MAXTIME);
    drv_play(b.data, b.len, 0);
    run_until_finished(MAXTIME);
    run_ms(1500);
    ea = expected(&a, VOL_FULL, 0);
    eb = expected(&b, VOL_FULL, 0);
    pad_a = ((size_t)a.frames * 4 % PERIOD) ? PERIOD - (size_t)a.frames * 4 % PERIOD : 0;
    check_stream("first track", ea, (size_t)a.frames * 4, 0, L);
    check_zero("pad", (size_t)a.frames * 4, (size_t)a.frames * 4 + pad_a, L);
    check_stream("second track", eb, (size_t)b.frames * 4,
                 (size_t)a.frames * 4 + pad_a, L);
    CHECK(V32(V_ST_TRACKS) == 2 && L->revoked == 0, "statistics");
    free(ea);
    free(eb);

    /* a new track replaces a playing one: what the card has is played first */
    if (!fresh())
        return;
    drv_play(a.data, a.len, 0);
    L = L1();
    run_ms(300);
    w_stop = V32(V_WRLO);
    drv_play(b.data, b.len, 0);
    CHECK(V32(V_ST_TRACKS) == 2 && V8(V_STATE) == ST_PLAY, "replace");
    run_until_finished(MAXTIME);
    run_ms(1500);
    ea = expected(&a, VOL_FULL, 0);
    eb = expected(&b, VOL_FULL, 0);
    CHECK(finished_count == 1, "only the second track ends");
    check_stream("replaced track", ea, w_stop, 0, L);
    check_stream("new track", eb, (size_t)b.frames * 4, w_stop, L);
    free(ea);
    free(eb);
    free(a.data);
    free(b.data);
}

static void test_short(void)
{
    struct wav w1 = simple_wav(2, 16, 100);
    struct wav w2 = simple_wav(2, 16, 1);
    struct wav w3 = simple_wav(2, 16, 882 * 7);        /* 7 whole periods */
    struct lease *L;
    u8 *e;

    /* a track shorter than the preroll of the firmware must still play */
    if (!fresh())
        return;
    drv_play(w1.data, w1.len, 0);
    L = L1();
    run_until_finished(1000);
    run_ms(1000);
    e = expected(&w1, VOL_FULL, 0);
    check_stream("100 frames", e, 400, 0, L);
    CHECK(L->outlen == 7 * PERIOD, "short track output %zu", L->outlen);
    check_zero("100 frames pad", 400, L->outlen, L);
    free(e);
    drv_play(w2.data, w2.len, 0);
    run_until_finished(1000);
    run_ms(1000);
    e = expected(&w2, VOL_FULL, 0);
    check_stream("1 frame", e, 4, 7 * PERIOD, L);
    CHECK(L->outlen == 8 * PERIOD, "second short track output %zu", L->outlen);
    free(e);

    play_and_check("whole periods", &w3, VOL_FULL, 0, 20);
    free(w1.data);
    free(w2.data);
    free(w3.data);
}

static void expect_refused(const char *what, const struct wav *w, u32 len)
{
    if (!fresh())
        return;
    drv_play(w->data, len, 0);
    CHECK(V8(V_FINPEND) != 0 && V8(V_STATE) == ST_IDLE, "%s: not refused", what);
    CHECK(V32(V_WRLO) == 0 && V32(V_ST_REFUSED) == 1, "%s: written", what);
    run_ms(100);
    CHECK(finished_count == 1, "%s: the end was reported %d times", what,
          finished_count);
    CHECK(lease[1].outlen == 0, "%s: audio played", what);
}

/* A refused track leaves the lease paused: with the card interrupt off
 * nothing publishes a heartbeat, and only a paused lease has no timeout. */
static void test_refusal_pauses(void)
{
    struct wavopt o;
    struct wav good = simple_wav(2, 16, 44100 * 3);
    struct wav shortw = simple_wav(2, 16, 4410);
    struct wav bad;
    struct lease *L;
    size_t before;

    wavopt_default(&o, 2, 16, 1000);
    o.rate = 48000;
    bad = make_wav(&o);

    /* an unsupported track replaces a playing one */
    if (!fresh())
        return;
    drv_play(good.data, good.len, 0);
    L = L1();
    run_ms(700);
    CHECK(be32(L->ctrl + 20) == 0, "playing is unpaused");
    drv_play(bad.data, bad.len, 0);
    CHECK(be32(L->ctrl + 20) == 1, "a refused track pauses the lease");
    run_ms(200);
    CHECK(finished_count == 1 && !hw_enabled, "refusal reported, interrupt off");
    run_ms(10000);
    CHECK(L->revoked == 0 && V8(V_DEAD) == 0, "lease revoked after a refusal: %d",
          L->revoked);
    before = L->outlen;
    drv_play(shortw.data, shortw.len, 0);
    run_until_finished(MAXTIME);
    run_ms(1500);
    CHECK(L->revoked == 0 && L->outlen > before, "a valid track plays after the refusal");
    drv_exit();

    /* an unsupported track straight after a finished one (the callback of
     * the sequence), then 10 s idle and a valid track */
    if (!fresh())
        return;
    drv_play(shortw.data, shortw.len, 0);
    L = L1();
    run_until_finished(MAXTIME);
    drv_play(bad.data, bad.len, 0);
    CHECK(be32(L->ctrl + 20) == 1, "refused after a finished track: paused");
    run_ms(10000);
    CHECK(finished_count == 2, "refusal reported (%d)", finished_count);
    CHECK(L->revoked == 0 && V8(V_DEAD) == 0, "lease revoked after 10 s idle: %d",
          L->revoked);
    before = L->outlen;
    drv_play(shortw.data, shortw.len, 0);
    run_until_finished(MAXTIME);
    run_ms(1500);
    CHECK(L->revoked == 0 && L->outlen > before, "a valid track plays after 10 s idle");
    drv_exit();
    free(good.data);
    free(shortw.data);
    free(bad.data);
}

static void test_parser(void)
{
    struct wavopt o;
    struct wav w;
    u8 *e;

    wavopt_default(&o, 2, 16, 1000);
    o.rate = 48000;
    w = make_wav(&o);
    expect_refused("48 kHz", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 1000);
    o.tag = 3;
    w = make_wav(&o);
    expect_refused("float", &w, w.len);
    free(w.data);
    wavopt_default(&o, 3, 16, 1000);
    w = make_wav(&o);
    expect_refused("3 channels", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 24, 1000);
    w = make_wav(&o);
    expect_refused("24 bit", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 1000);
    o.blockalign = 2;
    w = make_wav(&o);
    expect_refused("block align", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 1000);
    o.skip_fmt = 1;
    w = make_wav(&o);
    expect_refused("no fmt chunk", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 1000);
    o.fmt_after_data = 1;
    w = make_wav(&o);
    expect_refused("fmt behind data", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 0);
    w = make_wav(&o);
    expect_refused("no frames", &w, w.len);
    free(w.data);
    wavopt_default(&o, 2, 16, 1000);
    w = make_wav(&o);
    memcpy(w.data, "RIFX", 4);
    expect_refused("RIFX", &w, w.len);
    memcpy(w.data, "RIFF", 4);
    memcpy(w.data + 8, "AVI ", 4);
    expect_refused("not WAVE", &w, w.len);
    memcpy(w.data + 8, "WAVE", 4);
    expect_refused("11 bytes", &w, 11);
    expect_refused("header only", &w, 12);
    expect_refused("cut in the fmt chunk", &w, 12 + 8 + 10);
    expect_refused("cut behind the fmt chunk", &w, 12 + 24);
    expect_refused("cut in the data header", &w, 12 + 24 + 6);
    expect_refused("data header only", &w, 12 + 24 + 8);
    expect_refused("one byte of data", &w, 12 + 24 + 9);
    free(w.data);

    /* a chunk which is larger than the file, before the data */
    wavopt_default(&o, 2, 16, 1000);
    o.list_bytes = 4;
    w = make_wav(&o);
    le32(w.data + 12 + 24 + 4, 0x7fffffff);
    expect_refused("chunk size beyond the file", &w, w.len);
    le32(w.data + 12 + 24 + 4, 0xffffffffu);
    expect_refused("chunk size 0xffffffff", &w, w.len);
    free(w.data);

    /* accepted: odd chunk before the data, chunk behind the data, streamed
     * size fields, a file which is cut inside the data */
    wavopt_default(&o, 2, 16, 20000);
    o.list_bytes = 5;
    o.trailing_chunk = 1;
    w = make_wav(&o);
    play_and_check("odd LIST chunk and trailing chunk", &w, VOL_FULL, 0, 20);
    free(w.data);
    wavopt_default(&o, 2, 16, 20000);
    o.data_size_mode = 1;
    w = make_wav(&o);
    w.len = 12 + 24 + 8 + 20000 * 4;
    play_and_check("data size 0xffffffff", &w, VOL_FULL, 0, 20);
    free(w.data);
    wavopt_default(&o, 2, 16, 20000);
    o.data_size_mode = 2;
    w = make_wav(&o);
    w.len = 12 + 24 + 8 + 20000 * 4;
    play_and_check("data size beyond the file", &w, VOL_FULL, 0, 20);
    free(w.data);
    wavopt_default(&o, 2, 16, 20000);
    w = make_wav(&o);
    w.len = 12 + 24 + 8 + 19999 * 4 + 3;
    w.frames = 19999;
    play_and_check("file cut inside a frame", &w, VOL_FULL, 0, 20);
    free(w.data);
    wavopt_default(&o, 1, 8, 20000);
    o.list_bytes = 1;
    w = make_wav(&o);
    play_and_check("8 bit mono behind an odd chunk", &w, 0x3000, 0, 20);
    free(w.data);
    (void)e;
}

static void test_os_active(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 12);
    struct lease *L;

    /* WHDLoad loads for 10 s with the operating system active and nothing of
     * the plugin runs: suspend paused the lease, the firmware does not time
     * it out, and resume continues the track */
    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(1000);
    drv_suspend();
    CHECK(!hw_enabled && hw16() == 0x80, "suspend disables the card interrupt");
    CHECK(be32(L->ctrl + 20) == 1, "suspend pauses the lease");
    os_active = 1;
    run_ms(10000);
    CHECK(L->revoked == 0, "the lease was revoked in %d", L->revoked);
    os_active = 0;
    drv_resume();
    CHECK(hw_enabled && hw16() == 0x82, "resume enables the card interrupt");
    CHECK(be32(L->ctrl + 20) == 0, "resume unpauses a playing lease");
    CHECK(V8(V_ARMED) != 0, "armed while playing");
    run_until_finished(MAXTIME);
    run_ms(1500);
    CHECK(L->revoked == 0 && V8(V_DEAD) == 0, "lease intact");
    CHECK(L->W == ((u64)V32(V_WRHI) << 32 | V32(V_WRLO)), "cursor");
    CHECK(V32(V_ST_SUSP) == 1 && V32(V_ST_RESUME) == 1, "suspend and resume counters");
    drv_exit();
    CHECK(traced("intact") && traced("suspend 1, resume 1"), "exit trace");

    /* from init to the first track nothing runs for 10 s, not even an
     * interrupt: the lease is paused since init; a resume while idle keeps it
     * paused */
    if (!fresh())
        return;
    L = L1();
    run_ms(10000);
    CHECK(L->revoked == 0, "idle lease revoked in %d", L->revoked);
    drv_resume();
    CHECK(be32(L->ctrl + 20) == 1, "resume while idle stays paused");
    CHECK(!hw_enabled && hw16() == 0, "resume while idle leaves the card interrupt alone");
    run_ms(5000);
    CHECK(L->revoked == 0, "idle lease after resume revoked in %d", L->revoked);
    drv_play(w.data, w.len, 0);
    run_ms(500);
    CHECK(L->outlen > 0 && V8(V_DEAD) == 0, "plays after a long idle time");
    drv_stop();
    run_ms(10000);
    CHECK(L->revoked == 0, "stopped lease revoked in %d", L->revoked);

    /* the level 2 interrupt is masked for 3 s while a track plays: the
     * firmware revokes the lease, the driver notices and ends the track */
    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(500);
    os_active = 1;
    run_ms(3000);
    CHECK(L->revoked == 1, "revoked by the firmware: %d", L->revoked);
    os_active = 0;
    run_ms(100);
    CHECK(V8(V_DEAD) != 0 && V32(V_DEADSTAT) == 1, "dead %u status %u",
          V8(V_DEAD), V32(V_DEADSTAT));
    CHECK(finished_count == 1 && V8(V_STATE) == ST_IDLE, "end of the track");
    drv_play(w.data, w.len, 0);
    run_ms(100);
    CHECK(finished_count == 2 && V32(V_ST_REFUSED) == 1, "play after the loss");
    drv_exit();
    CHECK(traced("lost") && traced("status 1"), "exit trace %s",
          tracelog[ntrace - 1]);
    CHECK(released[1] == 1, "released anyway");
    free(w.data);
}

static void test_dead_midtrack(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 5);
    struct lease *L;
    u64 wr;

    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(700);
    fw_revoke(L, 2);                      /* a cursor fault of the firmware */
    run_ms(100);
    wr = V32(V_WRLO);
    CHECK(V8(V_DEAD) != 0 && V32(V_DEADSTAT) == 2, "dead %u status %u",
          V8(V_DEAD), V32(V_DEADSTAT));
    CHECK(finished_count == 1, "the end is reported once");
    run_ms(500);
    CHECK(V32(V_WRLO) == wr, "writing after the loss");
    free(w.data);
}

/* a credit line of another generation: the lease is lost, also with status OK */
static void test_generation(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 5);
    struct lease *L;

    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(500);
    CHECK(V8(V_DEAD) == 0, "lease intact");
    put32(L->ctrl + 64 + 4, L->gen + 1);
    run_ms(100);
    CHECK(V8(V_DEAD) != 0 && finished_count == 1, "generation mismatch: dead %u",
          V8(V_DEAD));
    free(w.data);
}

static void test_torn_and_stall(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 8);
    struct lease *L;
    u32 wr, hb;

    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    L = L1();
    run_ms(500);

    /* the line of the firmware stays in an update: no data, heartbeat goes on */
    L->torn = 1;
    run_ms(100);
    wr = V32(V_WRLO);
    hb = V32(V_HB);
    run_ms(400);
    CHECK(V32(V_WRLO) == wr, "written while the credit line is torn");
    CHECK(V32(V_HB) > hb + 10, "heartbeat while the credit line is torn");
    CHECK(V8(V_DEAD) == 0, "a torn line is not a lost lease");
    L->torn = 0;
    put32(L->ctrl + 64, (be32(L->ctrl + 64) + 1) & ~1u);
    run_ms(500);
    CHECK(V32(V_WRLO) > wr, "writing resumes");

    /* the card stops consuming: the lead stays within bounds */
    L->stall = 1;
    run_ms(2000);
    CHECK(L->max_lead <= LEAD, "lead %llu with a stalled consumer",
          (unsigned long long)L->max_lead);
    CHECK(L->revoked == 0, "revoked while stalled");
    L->stall = 0;
    run_until_finished(MAXTIME);
    run_ms(1500);
    CHECK(L->revoked == 0 && L->max_lead <= LEAD, "after the stall");
    free(w.data);
}

static void test_irq_filter(void)
{
    struct wav w = simple_wav(2, 16, 44100);
    u32 seq, irqs;
    struct lease *L;
    u32 r;

    if (!fresh())
        return;
    L = L1();
    /* not armed: the interrupt is none of ours */
    seq = be32(L->ctrl);
    set_hw16(0x0004);
    r = call(C_IRQ, 0, 0, NULL);
    CHECK(r == 0 && hw16() == 0x0004 && be32(L->ctrl) == seq,
          "irq while not armed");
    drv_play(w.data, w.len, 0);
    seq = be32(L->ctrl);
    irqs = V32(V_ST_IRQS);
    /* armed, but the vblank bit is not set (other card or CIA interrupt) */
    set_hw16(0x0008);
    r = call(C_IRQ, 0, 0, NULL);
    CHECK(r == 0 && hw16() == 0x0008 && be32(L->ctrl) == seq &&
          V32(V_ST_IRQS) == irqs, "foreign interrupt");
    set_hw16(0x0004);
    r = call(C_IRQ, 0, 0, NULL);
    CHECK(hw16() == 0x48 && be32(L->ctrl) != seq && V32(V_ST_IRQS) == irqs + 1,
          "own interrupt");
    free(w.data);
}

/* WHDLoad quits: the orders in which it may call the plugin. The card
 * interrupt must be off before the operating system is back. */
static void test_quit_order(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 4);
    struct wav s = simple_wav(2, 16, 500);
    int v;

    for (v = 0; v < 8; v++) {
        if (!fresh())
            return;
        if (v != 0)
            drv_play(v == 7 ? s.data : w.data, v == 7 ? s.len : w.len, v == 4);
        run_ms(v == 7 ? 600 : 700);
        if (v == 1 || v == 3 || v == 4) {
            drv_stop();
            CHECK(!hw_enabled && hw16() == 0x80, "variant %d: stop", v);
        }
        if (v == 2 || v == 3) {
            drv_suspend();
            CHECK(!hw_enabled && hw16() == 0x80, "variant %d: suspend", v);
        }
        if (v == 5) {
            drv_suspend();
            drv_resume();       /* resumed and quit while playing */
            CHECK(hw_enabled, "variant %d: resumed", v);
        }
        if (v == 6) {
            drv_stop();
            drv_suspend();
            drv_resume();
            CHECK(!hw_enabled, "variant %d: resume of a stopped lease", v);
        }
        /* v 0: idle; 2, 5: playing at quit, exit alone has to cope; 7: a
         * short track that has finished and drained */
        drv_exit();
        CHECK(hw16() == 0x80 && !hw_enabled && V8(V_ARMED) == 0,
              "variant %d: card interrupt after exit", v);
        CHECK(released[1] == 1 && closes[2] == 1 && traced("lease"),
              "variant %d: exit did not finish", v);
    }
    free(w.data);
    free(s.data);
}

static void test_exit_midtrack(void)
{
    struct wav w = simple_wav(2, 16, 44100 * 3);

    if (!fresh())
        return;
    drv_play(w.data, w.len, 0);
    run_ms(300);
    drv_stop();
    drv_exit();
    CHECK(hw16() == 0x80 && !hw_enabled, "interrupt disabled on exit");
    CHECK(V8(V_LIVE) == 0, "state after exit");
    CHECK(rem_calls == 0 && released[1] == 1 && closes[2] == 1, "cleanup");
    CHECK(traced("tracks 1, refused 0") && traced("intact"), "exit trace %s",
          tracelog[ntrace - 1]);
    CHECK(traced("suspend 0, resume 0"), "exit trace %s", tracelog[ntrace - 1]);
    free(w.data);
}

/* WAV files written by tools/make-test-tracks.py (Python wave module):
 * the canonical 44 byte header */
static void test_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    struct wav w;
    u32 rate, dsize;

    if (!f) {
        perror(path);
        failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    w.data = aligned_alloc(4, ((size_t)size + 16 + 3) & ~3u);
    memset(w.data, 0, (size_t)size + 16);
    if (fread(w.data, 1, (size_t)size, f) != (size_t)size) {
        perror(path);
        failures++;
        fclose(f);
        return;
    }
    fclose(f);
    w.len = (u32)size;
    w.ch = w.data[22];
    w.bits = w.data[34];
    rate = w.data[24] | (w.data[25] << 8) | (w.data[26] << 16);
    dsize = w.data[40] | (w.data[41] << 8) | (w.data[42] << 16) |
            ((u32)w.data[43] << 24);
    w.pcm = w.data + 44;
    w.frames = dsize / ((u32)w.ch * w.bits / 8);
    CHECK(!memcmp(w.data, "RIFF", 4) && !memcmp(w.data + 36, "data", 4),
          "%s: header", path);
    if (rate == 44100)
        play_and_check(path, &w, VOL_FULL, 0, 20);
    else
        expect_refused(path, &w, w.len);
    free(w.data);
}

int main(int argc, char **argv)
{
    int i;

    FILE *f;
    long size;
    u8 *file;

    if (argc < 2) {
        fprintf(stderr, "usage: %s harness.bin [track.wav ...]\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)size);
    if (fread(file, 1, (size_t)size, f) != (size_t)size) {
        perror("read");
        return 2;
    }
    fclose(f);
    blob = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (blob == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    memcpy(blob, file, (size_t)size);
    ctl = (u32 *)(blob + *(u32 *)blob);
    vars = blob + ctl[C_VARS];
    world_setup();

    test_init();
    test_play();
    test_formats();
    test_volume_change();
    test_loop_stop();
    test_sequence_and_replace();
    test_short();
    test_parser();
    test_refusal_pauses();
    test_os_active();
    test_dead_midtrack();
    test_generation();
    test_torn_and_stall();
    test_irq_filter();
    test_exit_midtrack();
    test_quit_order();
    for (i = 2; i < argc; i++)
        test_file(argv[i]);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
