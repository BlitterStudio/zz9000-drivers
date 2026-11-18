/*
 * Host tests of the ZZRun service handlers: a fake request stream goes into
 * zzrun_serve_request(), a fake DOS (an in-memory volume with locks, file
 * handles and lists), fake console, display and audio backends stand under it,
 * and the completions are checked.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzrun_serve.h"

/* ---- The fake volume ---------------------------------------------------- */

#define MAX_NODES 40
#define MAX_LOCKS 128
#define MAX_FH 40
#define MAX_LISTS 40
#define FILE_CAP 65536u

struct fnode {
    int used;
    int parent;
    char name[24];
    int is_dir;
    int link;           /* entry resolves to this node (a soft link), or -1 */
    int held;           /* another program holds it: locks fail IN_USE */
    uint8_t *data;
    uint32_t size;
    uint32_t mtime_days;
};

struct flock {
    int used;
    int node;
};

struct ffh {
    int used;
    int node;
    uint32_t pos;
};

struct flist {
    int used;
    int lock_node;
    int next_child; /* node index scan position */
};

struct fake {
    struct fnode nodes[MAX_NODES];
    struct flock locks[MAX_LOCKS];
    struct ffh fhs[MAX_FH];
    struct flist lists[MAX_LISTS];
    int live_locks;
    int live_fhs;
    int live_lists;

    int live;            /* the request is still wanted */
    int broken;          /* a Ctrl-C is pending */
    int break_after_open; /* set `broken` once an open succeeded */
    int set_size_unsupported;
    int close_undelivered;
    int fail_transport_lock; /* lock_rel of this leaf name loses its packet */
    char fail_transport_name[24];

    /* a log of what the handlers asked of DOS */
    int opens[3];        /* per zzrun_mode */
    int probes;
    int set_sizes;
    int end_or_close;
    int reads;
    int writes;
    int max_read;
    int lock_rel_calls;

    /* target state */
    uintptr_t t_dir;
    char t_name[256];
};

static struct fake fk;

static int node_new(int parent, const char *name, int is_dir)
{
    int i;

    for (i = 0; i < MAX_NODES; i++) {
        if (!fk.nodes[i].used) {
            struct fnode *n = &fk.nodes[i];

            memset(n, 0, sizeof(*n));
            n->used = 1;
            n->parent = parent;
            n->link = -1;
            n->is_dir = is_dir;
            n->mtime_days = 1000u + (uint32_t)i;
            strncpy(n->name, name, sizeof(n->name) - 1u);
            if (!is_dir)
                n->data = calloc(1, FILE_CAP);
            return i;
        }
    }
    assert(!"out of nodes");
    return -1;
}

static int file_new(int parent, const char *name, const char *text)
{
    int n = node_new(parent, name, 0);

    fk.nodes[n].size = (uint32_t)strlen(text);
    memcpy(fk.nodes[n].data, text, fk.nodes[n].size);
    return n;
}

static int child_of(int dir, const char *name)
{
    int i;

    for (i = 0; i < MAX_NODES; i++) {
        struct fnode *n = &fk.nodes[i];

        if (n->used && n->parent == dir && strcmp(n->name, name) == 0)
            return n->link >= 0 ? n->link : i;
    }
    return -1;
}

static uintptr_t lock_new(int node)
{
    int i;

    for (i = 0; i < MAX_LOCKS; i++) {
        if (!fk.locks[i].used) {
            fk.locks[i].used = 1;
            fk.locks[i].node = node;
            fk.live_locks++;
            return (uintptr_t)(i + 1);
        }
    }
    assert(!"out of locks");
    return 0;
}

static int lock_node(uintptr_t lock)
{
    assert(lock >= 1 && lock <= MAX_LOCKS && fk.locks[lock - 1].used);
    return fk.locks[lock - 1].node;
}

static void data_free_all(void)
{
    int i;

    for (i = 0; i < MAX_NODES; i++)
        free(fk.nodes[i].data);
}

/* ---- zzrun_dos over the fake volume ------------------------------------- */

static int f_live(void *ctx)
{
    (void)ctx;
    return fk.live;
}

static int f_user_break(void *ctx)
{
    (void)ctx;
    return fk.broken;
}

static uintptr_t f_dup_lock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return lock_new(lock_node(lock));
}

static void f_unlock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    (void)lock_node(lock);
    fk.locks[lock - 1].used = 0;
    fk.live_locks--;
}

static uintptr_t f_parent_dir(void *ctx, uintptr_t lock)
{
    int parent = fk.nodes[lock_node(lock)].parent;

    (void)ctx;
    return parent < 0 ? 0 : lock_new(parent);
}

static int f_same_lock(void *ctx, uintptr_t a, uintptr_t b)
{
    (void)ctx;
    return lock_node(a) == lock_node(b);
}

static uintptr_t f_lock_rel(void *ctx, uintptr_t dir, const char *name,
                            int *transport_ok, int32_t *io)
{
    int n;

    (void)ctx;
    fk.lock_rel_calls++;
    *io = 0;
    if (fk.fail_transport_lock && strcmp(name, fk.fail_transport_name) == 0) {
        *transport_ok = 0;
        return 0;
    }
    *transport_ok = 1;
    n = child_of(lock_node(dir), name);
    if (n < 0) {
        *io = ZZRUN_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    if (fk.nodes[n].held) {
        *io = ZZRUN_IOERR_OBJECT_IN_USE;
        return 0;
    }
    return lock_new(n);
}

/* "Vol:a/b": walks from node 0. */
static int abs_find(const char *name)
{
    const char *colon = strchr(name, ':');
    char comp[24];
    int cur = 0;
    const char *p;

    if (!colon || strncmp(name, fk.nodes[0].name, (size_t)(colon - name)) != 0)
        return -1;
    p = colon + 1;
    while (*p) {
        size_t n = 0;

        while (*p && *p != '/')
            comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/')
            p++;
        cur = child_of(cur, comp);
        if (cur < 0)
            return -1;
    }
    return cur;
}

static uintptr_t f_lock_abs(void *ctx, const char *name, int *transport_ok,
                            int32_t *io)
{
    int n = abs_find(name);

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (n < 0) {
        *io = ZZRUN_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    return lock_new(n);
}

static void info_from(int n, struct zzrun_info *info)
{
    struct fnode *node = &fk.nodes[n];

    memset(info, 0, sizeof(*info));
    info->is_dir = node->is_dir;
    info->size = node->size;
    info->mtime = zzrun_amiga_unix(node->mtime_days, 0u, 0u);
    strcpy(info->name, node->name);
}

static int f_lock_examine(void *ctx, uintptr_t lock, struct zzrun_info *info,
                          int *transport_ok, int32_t *io)
{
    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    info_from(lock_node(lock), info);
    return 1;
}

static int target_node(int *held)
{
    int n;

    if (fk.t_dir)
        n = child_of(lock_node(fk.t_dir), fk.t_name);
    else
        n = abs_find(fk.t_name);
    *held = n >= 0 && fk.nodes[n].held;
    return n;
}

static void f_target_begin(void *ctx, uintptr_t dir, const char *name)
{
    (void)ctx;
    fk.t_dir = dir;
    strncpy(fk.t_name, name, sizeof(fk.t_name) - 1u);
}

static int f_target_probe(void *ctx, int *exists, int *transport_ok,
                          int32_t *io)
{
    int held;

    (void)ctx;
    fk.probes++;
    *transport_ok = 1;
    *io = 0;
    *exists = target_node(&held) >= 0;
    return 1;
}

static int fh_new(int node)
{
    int i;

    for (i = 0; i < MAX_FH; i++) {
        if (!fk.fhs[i].used) {
            fk.fhs[i].used = 1;
            fk.fhs[i].node = node;
            fk.fhs[i].pos = 0;
            fk.live_fhs++;
            return i + 1;
        }
    }
    assert(!"out of handles");
    return 0;
}

static uintptr_t f_target_open(void *ctx, enum zzrun_mode mode,
                               int *transport_ok, int32_t *io)
{
    int held;
    int n;

    (void)ctx;
    fk.opens[mode]++;
    *transport_ok = 1;
    *io = 0;
    n = target_node(&held);
    if (n >= 0 && held) {
        *io = ZZRUN_IOERR_OBJECT_IN_USE;
        return 0;
    }
    if (mode == ZZRUN_MODE_OLD && n < 0) {
        *io = ZZRUN_IOERR_OBJECT_NOT_FOUND;
        return 0;
    }
    if (n < 0) {
        /* MODE_READWRITE and MODE_NEWFILE create a missing file. */
        int parent = fk.t_dir ? lock_node(fk.t_dir) : 0;

        if (mode == ZZRUN_MODE_READWRITE) {
            *io = ZZRUN_IOERR_OBJECT_NOT_FOUND;
            return 0;
        }
        n = node_new(parent, fk.t_name, 0);
    } else if (mode == ZZRUN_MODE_NEW) {
        fk.nodes[n].size = 0;
    }
    if (fk.break_after_open)
        fk.broken = 1;
    return (uintptr_t)fh_new(n);
}

static void f_target_end(void *ctx)
{
    (void)ctx;
    fk.t_dir = 0;
}

static struct ffh *fh_get(uintptr_t fh)
{
    assert(fh >= 1 && fh <= MAX_FH && fk.fhs[fh - 1].used);
    return &fk.fhs[fh - 1];
}

static int32_t f_fh_set_size0(void *ctx, uintptr_t fh, int *transport_ok,
                              int32_t *io)
{
    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    fk.set_sizes++;
    if (fk.set_size_unsupported) {
        *io = ZZRUN_IOERR_ACTION_NOT_KNOWN;
        return -1;
    }
    fk.nodes[fh_get(fh)->node].size = 0;
    return 0;
}

static int32_t f_fh_read(void *ctx, uintptr_t fh, uint8_t *dst, uint32_t bytes,
                         int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];
    uint32_t left = h->pos < n->size ? n->size - h->pos : 0u;

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    fk.reads++;
    if (bytes > (uint32_t)fk.max_read)
        fk.max_read = (int)bytes;
    if (left > bytes)
        left = bytes;
    memcpy(dst, n->data + h->pos, left);
    h->pos += left;
    return (int32_t)left;
}

static int32_t f_fh_write(void *ctx, uintptr_t fh, const uint8_t *src,
                          uint32_t bytes, int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    fk.writes++;
    if (h->pos + bytes > FILE_CAP) {
        *io = ZZRUN_IOERR_DISK_FULL;
        return -1;
    }
    memcpy(n->data + h->pos, src, bytes);
    h->pos += bytes;
    if (h->pos > n->size)
        n->size = h->pos;
    return (int32_t)bytes;
}

static int32_t f_fh_seek(void *ctx, uintptr_t fh, int32_t offset, int whence,
                         int *transport_ok, int32_t *io)
{
    struct ffh *h = fh_get(fh);
    struct fnode *n = &fk.nodes[h->node];
    int64_t base = whence == 0 ? 0 : (whence == 1 ? h->pos : n->size);
    int64_t target = base + offset;
    int32_t old = (int32_t)h->pos;

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (target < 0 || target > (int64_t)n->size) {
        *io = ZZRUN_IOERR_SEEK_ERROR;
        return -1;
    }
    h->pos = (uint32_t)target;
    return old;
}

static void f_fh_end_or_close(void *ctx, uintptr_t fh)
{
    (void)ctx;
    (void)fh_get(fh);
    fk.fhs[fh - 1].used = 0;
    fk.live_fhs--;
    fk.end_or_close++;
}

static int f_fh_close(void *ctx, uintptr_t fh, int *delivered, int32_t *io)
{
    (void)ctx;
    *io = 0;
    if (fk.close_undelivered) {
        *delivered = 0;
        return 0;
    }
    *delivered = 1;
    fk.fhs[fh_get(fh) - fk.fhs].used = 0;
    fk.live_fhs--;
    return 1;
}

static int f_list_open(void *ctx, uintptr_t lock, uintptr_t *list,
                       int *transport_ok, int32_t *io)
{
    int i;
    int n = lock_node(lock);

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (!fk.nodes[n].is_dir) {
        *io = ZZRUN_IOERR_OBJECT_WRONG_TYPE;
        return 0;
    }
    for (i = 0; i < MAX_LISTS; i++) {
        if (!fk.lists[i].used) {
            fk.lists[i].used = 1;
            fk.lists[i].lock_node = n;
            fk.lists[i].next_child = 0;
            fk.live_lists++;
            /* the list owns the lock */
            f_unlock(NULL, lock);
            *list = (uintptr_t)(i + 1);
            return 1;
        }
    }
    assert(!"out of lists");
    return 0;
}

static int f_list_next(void *ctx, uintptr_t list, struct zzrun_info *info,
                       int *transport_ok, int32_t *io)
{
    struct flist *l;

    (void)ctx;
    assert(list >= 1 && list <= MAX_LISTS && fk.lists[list - 1].used);
    l = &fk.lists[list - 1];
    *transport_ok = 1;
    *io = 0;
    while (l->next_child < MAX_NODES) {
        int i = l->next_child++;

        if (fk.nodes[i].used && fk.nodes[i].parent == l->lock_node) {
            info_from(i, info);
            {
                /* A real ACTION_EXAMINE_NEXT result: counted name, with no
                 * NUL and unrelated bytes after its counted end. */
                uint8_t fib_name[108];
                size_t n = strlen(fk.nodes[i].name);

                assert(n < sizeof(fib_name));
                memset(fib_name, 0xa5, sizeof(fib_name));
                fib_name[0] = (uint8_t)n;
                memcpy(fib_name + 1, fk.nodes[i].name, n);
                zzrun_bstr_decode(fib_name, sizeof(fib_name),
                                  info->name, sizeof(info->name));
            }
            return 1;
        }
    }
    *io = ZZRUN_IOERR_NO_MORE_ENTRIES;
    return 0;
}

static void f_list_close(void *ctx, uintptr_t list)
{
    (void)ctx;
    assert(list >= 1 && list <= MAX_LISTS && fk.lists[list - 1].used);
    fk.lists[list - 1].used = 0;
    fk.live_lists--;
}

static const struct zzrun_dos fake_dos = {
    NULL, f_live, f_user_break, f_dup_lock, f_unlock, f_parent_dir,
    f_same_lock, f_lock_rel, f_lock_abs, f_lock_examine, f_target_begin,
    f_target_probe, f_target_open, f_target_end, f_fh_set_size0, f_fh_read,
    f_fh_write, f_fh_seek, f_fh_end_or_close, f_fh_close, f_list_open,
    f_list_next, f_list_close
};

/* ---- Completions the sink collects -------------------------------------- */

#define MAX_SINK 16

struct sunk {
    struct zzrun_req req;
    struct zzrun_done done;
};

static struct sunk sunk[MAX_SINK];
static int sunk_n;

static void sink_complete(void *ctx, const struct zzrun_req *req,
                          const struct zzrun_done *done)
{
    (void)ctx;
    assert(sunk_n < MAX_SINK);
    sunk[sunk_n].req = *req;
    sunk[sunk_n].done = *done;
    sunk_n++;
}

static const struct zzrun_sink sink = { NULL, sink_complete };

/* ---- Fixture: ROOT is Vol:root, with a sibling outside it --------------- */

enum { N_VOL = 0, N_ROOT, N_DIR, N_HELLO, N_OUTSIDE, N_SECRET };

static struct zzrun_serve serve;
static uint32_t next_id;

static void fixture(int abs_paths)
{
    int escape;

    if (fk.nodes[0].used)
        data_free_all();
    memset(&fk, 0, sizeof(fk));
    fk.live = 1;
    node_new(-1, "Vol", 1);                       /* 0 */
    node_new(0, "root", 1);                       /* 1 */
    node_new(1, "dir", 1);                        /* 2 */
    file_new(2, "hello.txt", "hello world");      /* 3 */
    node_new(0, "outside", 1);                    /* 4 */
    file_new(4, "secret.txt", "top secret");      /* 5 */
    file_new(1, "top.txt", "top");                /* 6 */
    escape = node_new(1, "escape", 0);            /* 7: a link out of ROOT */
    fk.nodes[escape].link = N_SECRET;
    zzrun_serve_init(&serve, &sink);
    zzrun_files_init(&serve.files, &fake_dos, 0, abs_paths);
    serve.files.root = lock_new(N_ROOT);
    sunk_n = 0;
    next_id = 100u;
}

static void fixture_free(void)
{
    zzrun_files_close_all(&serve.files);
    f_unlock(NULL, serve.files.root);
    assert(fk.live_locks == 0);
    assert(fk.live_fhs == 0);
    assert(fk.live_lists == 0);
}

/* ---- Request helpers ---------------------------------------------------- */

static struct zzrun_req mkreq(uint16_t op)
{
    struct zzrun_req r;

    memset(&r, 0, sizeof(r));
    r.op = op;
    r.id = next_id++;
    return r;
}

static int32_t serve_one(const struct zzrun_req *req, struct zzrun_done *done)
{
    int rc = zzrun_serve_request(&serve, req, done);

    assert(rc == ZZRUN_SERVE_DONE);
    return done->result;
}

static int32_t do_open(uint32_t fd, uint32_t flags, const char *path)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_OPEN);
    struct zzrun_done d;
    uint8_t buf[ZZRUN_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.arg[AAR_A_FILE_OPEN_FD] = fd;
    r.arg[AAR_A_FILE_OPEN_FLAGS] = flags;
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, &d);
}

static int32_t do_close(uint32_t fd)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_CLOSE);
    struct zzrun_done d;

    r.arg[AAR_A_FILE_FD] = fd;
    return serve_one(&r, &d);
}

static int32_t do_read(uint32_t fd, uint32_t len, uint8_t *buf,
                       uint32_t buf_len, uint32_t *count)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_READ);
    struct zzrun_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_LEN] = len;
    r.buf = buf;
    r.buf_len = buf_len;
    rc = serve_one(&r, &d);
    if (count)
        *count = d.out[AAR_R_FILE_COUNT];
    return rc;
}

static int32_t do_write(uint32_t fd, const uint8_t *data, uint32_t len,
                        uint32_t *count)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_WRITE);
    struct zzrun_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_LEN] = len;
    r.buf = (uint8_t *)(uintptr_t)data;
    r.buf_len = len;
    rc = serve_one(&r, &d);
    if (count)
        *count = d.out[AAR_R_FILE_COUNT];
    return rc;
}

static int32_t do_seek(uint32_t fd, int64_t offset, uint32_t whence,
                       uint32_t *pos)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_SEEK);
    struct zzrun_done d;
    int32_t rc;

    r.arg[AAR_A_FILE_FD] = fd;
    r.arg[AAR_A_FILE_OFF_HI] = (uint32_t)((uint64_t)offset >> 32);
    r.arg[AAR_A_FILE_OFF_LO] = (uint32_t)offset;
    r.arg[AAR_A_FILE_WHENCE] = whence;
    rc = serve_one(&r, &d);
    if (pos)
        *pos = d.out[AAR_R_FILE_POS_LO];
    return rc;
}

static int32_t do_stat(const char *path, struct zzrun_done *d)
{
    struct zzrun_req r = mkreq(AAR_OP_FILE_STAT);
    uint8_t buf[ZZRUN_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, d);
}

static int32_t do_list_open(uint32_t id, const char *path)
{
    struct zzrun_req r = mkreq(AAR_OP_LIST_OPEN);
    struct zzrun_done d;
    uint8_t buf[ZZRUN_PATH_BYTES + 8u];

    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, path);
    r.arg[AAR_A_LIST_ID] = id;
    r.buf = buf;
    r.buf_len = (uint32_t)strlen(path) + 1u;
    return serve_one(&r, &d);
}

static int32_t do_list_next(uint32_t id, char *name, uint32_t name_len,
                            uint32_t *mode)
{
    struct zzrun_req r = mkreq(AAR_OP_LIST_NEXT);
    struct zzrun_done d;
    uint8_t buf[64];
    int32_t rc;

    memset(buf, 0xee, sizeof(buf));
    r.arg[AAR_A_LIST_ID] = id;
    r.buf = buf;
    r.buf_len = name_len;
    rc = serve_one(&r, &d);
    if (rc == AAR_OK) {
        strcpy(name, (const char *)buf);
        *mode = d.out[AAR_R_LIST_MODE];
    }
    return rc;
}

static int32_t do_list_close(uint32_t id)
{
    struct zzrun_req r = mkreq(AAR_OP_LIST_CLOSE);
    struct zzrun_done d;

    r.arg[AAR_A_LIST_ID] = id;
    return serve_one(&r, &d);
}

#define R AAR_FILE_READ
#define W AAR_FILE_WRITE
#define C AAR_FILE_CREATE
#define T AAR_FILE_TRUNCATE
#define X AAR_FILE_EXCL

/* ---- File tests --------------------------------------------------------- */

static void test_read_in_chunks_and_past_the_end(void)
{
    uint8_t out[32];
    uint32_t count = 0;
    int node;
    uint32_t i;
    uint8_t *big;

    fixture(0);
    node = N_HELLO;
    for (i = 0; i < 20000u; i++)
        fk.nodes[node].data[i] = (uint8_t)(i * 7u);
    fk.nodes[node].size = 20000u;
    assert(do_open(3, R, "dir/hello.txt") == AAR_OK);

    big = malloc(40000u);
    assert(do_read(3, 20000u, big, 40000u, &count) == AAR_OK);
    assert(count == 20000u);
    for (i = 0; i < 20000u; i++)
        assert(big[i] == (uint8_t)(i * 7u));
    /* one DOS read per 8 KiB chunk, none larger */
    assert(fk.reads == 3 && fk.max_read == (int)ZZRUN_IO_CHUNK);
    /* at the end: a short read, then nothing */
    assert(do_read(3, 100u, out, sizeof(out), &count) == AAR_E_INVAL);
    assert(do_read(3, 16u, out, sizeof(out), &count) == AAR_OK && count == 0u);
    /* a length past the payload is refused */
    assert(do_seek(3, 0, AAR_SEEK_SET, NULL) == AAR_OK);
    assert(do_read(3, 4097u, out, 4096u, &count) == AAR_E_INVAL);
    assert(do_read(3, 11u, out, sizeof(out), &count) == AAR_OK && count == 11u);
    free(big);
    /* a bad descriptor */
    assert(do_read(4, 1u, out, sizeof(out), &count) == AAR_E_BADF);
    assert(do_read(2, 1u, out, sizeof(out), &count) == AAR_E_BADF);
    assert(do_read(3u + ZZRUN_FD_COUNT, 1u, out, sizeof(out), &count) ==
           AAR_E_BADF);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_read_stops_at_a_break_with_the_bytes_so_far(void)
{
    uint8_t big[20000];
    uint32_t count = 77u;

    fixture(0);
    fk.nodes[N_HELLO].size = 20000u;
    assert(do_open(3, R, "dir/hello.txt") == AAR_OK);
    fk.broken = 1;
    assert(do_read(3, 20000u, big, sizeof(big), &count) == AAR_OK);
    assert(count == 0u && fk.reads == 0);
    fk.broken = 0;
    fk.live = 0;
    assert(do_read(3, 100u, big, sizeof(big), &count) == AAR_E_IO);
    fk.live = 1;
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_write_seek_and_stat(void)
{
    static const uint8_t text[] = "0123456789";
    uint8_t back[16];
    uint32_t count = 0;
    uint32_t pos = 0;
    struct zzrun_done d;

    fixture(0);
    assert(do_open(5, W | C | T, "new.bin") == AAR_OK);
    assert(do_write(5, text, 10u, &count) == AAR_OK && count == 10u);
    assert(fk.nodes[child_of(N_ROOT, "new.bin")].size == 10u);
    assert(do_seek(5, 4, AAR_SEEK_SET, &pos) == AAR_OK && pos == 4u);
    assert(do_seek(5, 2, AAR_SEEK_CUR, &pos) == AAR_OK && pos == 6u);
    assert(do_seek(5, -3, AAR_SEEK_END, &pos) == AAR_OK && pos == 7u);
    /* past either end is the handler's refusal */
    assert(do_seek(5, 100, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, -100, AAR_SEEK_CUR, &pos) == AAR_E_INVAL);
    /* a SET offset above 2 GiB, a bad whence and a huge END offset */
    assert(do_seek(5, 0x80000000LL, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, -1, AAR_SEEK_SET, &pos) == AAR_E_INVAL);
    assert(do_seek(5, 0, 3u, &pos) == AAR_E_INVAL);
    assert(do_seek(5, 0x100000000LL, AAR_SEEK_END, &pos) == AAR_E_INVAL);
    assert(do_seek(6, 0, AAR_SEEK_SET, &pos) == AAR_E_BADF);
    assert(do_seek(5, 0, AAR_SEEK_SET, &pos) == AAR_OK && pos == 0u);
    assert(do_read(5, 10u, back, sizeof(back), &count) == AAR_OK && count == 10u);
    assert(memcmp(back, text, 10u) == 0);
    assert(do_close(5) == AAR_OK);
    assert(do_close(5) == AAR_E_BADF);

    assert(do_stat("new.bin", &d) == AAR_OK);
    assert(d.out[AAR_R_STAT_SIZE_LO] == 10u && d.out[AAR_R_STAT_SIZE_HI] == 0u);
    assert(d.out[AAR_R_STAT_MODE] == AAR_MODE_FILE);
    assert(d.out[AAR_R_STAT_MTIME_LO] ==
           zzrun_amiga_unix(fk.nodes[child_of(N_ROOT, "new.bin")].mtime_days,
                            0u, 0u));
    assert(do_stat("dir", &d) == AAR_OK);
    assert(d.out[AAR_R_STAT_MODE] == AAR_MODE_DIR);
    assert(do_stat("dir/nothing", &d) == AAR_E_NOENT);
    assert(do_stat("", &d) == AAR_E_INVAL);
    fixture_free();
}

static void test_each_open_plan_acts_on_the_file_it_should(void)
{
    uint8_t b[16];
    uint32_t count;
    int n;

    /* READ: the file must exist, MODE_OLDFILE, no probe. */
    fixture(0);
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(fk.opens[ZZRUN_MODE_OLD] == 1 && fk.probes == 0);
    assert(do_open(4, R, "missing") == AAR_E_NOENT);
    assert(do_close(3) == AAR_OK);

    /* WRITE: probes, never creates. */
    assert(do_open(3, W, "missing") == AAR_E_NOENT);
    assert(child_of(N_ROOT, "missing") < 0 && fk.opens[ZZRUN_MODE_READWRITE] == 0);
    assert(do_open(3, W, "top.txt") == AAR_OK);
    assert(fk.opens[ZZRUN_MODE_READWRITE] == 1);
    assert(do_read(3, 3u, b, sizeof(b), &count) == AAR_OK && count == 3u);
    assert(do_close(3) == AAR_OK);

    /* TRUNCATE without CREATE: SetFileSize(0) on an existing file only. */
    assert(do_open(3, W | T, "missing") == AAR_E_NOENT);
    assert(do_open(3, W | T, "top.txt") == AAR_OK);
    assert(fk.set_sizes == 1 && fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* ... and MODE_NEWFILE where the handler has no SetFileSize. */
    fixture_free();
    fixture(0);
    fk.set_size_unsupported = 1;
    assert(do_open(3, W | T, "top.txt") == AAR_OK);
    assert(fk.set_sizes == 1 && fk.opens[ZZRUN_MODE_NEW] == 1);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* CREATE alone keeps an existing file's contents, creates a missing one. */
    fixture_free();
    fixture(0);
    assert(do_open(3, W | C, "top.txt") == AAR_OK);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    assert(fk.opens[ZZRUN_MODE_NEW] == 0);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, W | C, "made.txt") == AAR_OK);
    n = child_of(N_ROOT, "made.txt");
    assert(n >= 0 && fk.nodes[n].size == 0u && fk.opens[ZZRUN_MODE_NEW] == 1);
    assert(do_close(3) == AAR_OK);

    /* CREATE|TRUNCATE replaces. */
    assert(do_open(3, W | C | T, "top.txt") == AAR_OK);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 0u);
    assert(do_close(3) == AAR_OK);

    /* The flags themselves: refused before DOS is touched. */
    fixture_free();
    fixture(0);
    assert(do_open(3, 0u, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, R | C, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, W | X, "top.txt") == AAR_E_INVAL);
    assert(do_open(3, R | 64u, "top.txt") == AAR_E_INVAL);
    assert(fk.lock_rel_calls == 0 && fk.probes == 0);
    fixture_free();
}

static void test_exclusive_create_refuses_an_existing_file(void)
{
    int n;

    fixture(0);
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_EXIST);
    assert(fk.opens[ZZRUN_MODE_NEW] == 0);
    /* held by another program: the lock is refused IN_USE, still EXIST */
    fk.nodes[child_of(N_ROOT, "top.txt")].held = 1;
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_EXIST);
    assert(fk.opens[ZZRUN_MODE_NEW] == 0);
    /* and one that is not there is created */
    assert(do_open(3, W | C | X, "fresh.txt") == AAR_OK);
    n = child_of(N_ROOT, "fresh.txt");
    assert(n >= 0 && fk.opens[ZZRUN_MODE_NEW] == 1);
    assert(do_open(4, W | C | X, "fresh.txt") == AAR_E_EXIST);
    assert(do_close(3) == AAR_OK);
    /* EXCL with TRUNCATE is still exclusive */
    assert(do_open(3, W | C | T | X, "fresh.txt") == AAR_E_EXIST);
    fixture_free();
}

static void test_root_confinement(void)
{
    uint32_t count;
    uint8_t b[16];

    fixture(0);
    /* inside, with dot and parent components that stay inside */
    assert(do_open(3, R, "dir/../top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "./dir/./hello.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "dir//hello.txt") == AAR_E_NOENT);
    assert(do_open(3, R, "dir//top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    /* out of it */
    assert(do_open(4, R, "../outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "dir/../../outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "/outside/secret.txt") == AAR_E_ACCES);
    assert(do_open(4, R, "..") == AAR_E_ACCES);
    assert(do_open(4, W | C, "../new.txt") == AAR_E_ACCES);
    assert(child_of(N_VOL, "new.txt") < 0);
    /* a soft link whose target is outside ROOT */
    assert(do_open(4, R, "escape") == AAR_E_ACCES);
    assert(do_open(4, W | C | X, "escape") == AAR_E_ACCES);
    assert(do_stat("escape", &(struct zzrun_done){ 0 }) == AAR_E_ACCES);
    assert(do_stat("../outside", &(struct zzrun_done){ 0 }) == AAR_E_ACCES);
    assert(do_list_open(3, "../outside") == AAR_E_ACCES);
    /* an absolute name needs ABS */
    assert(do_open(4, R, "Vol:outside/secret.txt") == AAR_E_ACCES);
    assert(do_stat("Vol:outside/secret.txt", &(struct zzrun_done){ 0 }) ==
           AAR_E_ACCES);
    assert(do_list_open(3, "Vol:outside") == AAR_E_ACCES);
    (void)count;
    (void)b;
    assert(fk.opens[ZZRUN_MODE_OLD] == 3);
    /* the doubled slash is the parent, so only a name of the parent resolves */
    fixture_free();

    /* with ABS an absolute name goes to the volume, outside ROOT or not */
    fixture(1);
    assert(do_open(3, R, "Vol:outside/secret.txt") == AAR_OK);
    assert(do_read(3, 10u, b, sizeof(b), &count) == AAR_OK && count == 10u);
    assert(memcmp(b, "top secret", 10u) == 0);
    assert(do_close(3) == AAR_OK);
    assert(do_open(3, R, "Vol:nothing") == AAR_E_NOENT);
    assert(do_stat("Vol:outside/secret.txt", &(struct zzrun_done){ 0 }) ==
           AAR_OK);
    /* relative names stay confined even with ABS */
    assert(do_open(3, R, "../outside/secret.txt") == AAR_E_ACCES);
    fixture_free();
}

static void test_transport_failures_are_io_errors_not_dos_results(void)
{
    fixture(0);
    fk.fail_transport_lock = 1;
    strcpy(fk.fail_transport_name, "dir");
    assert(do_open(3, R, "dir/hello.txt") == AAR_E_IO);
    strcpy(fk.fail_transport_name, "top.txt");
    assert(do_open(3, R, "top.txt") == AAR_E_IO);
    assert(do_open(3, W | C | X, "top.txt") == AAR_E_IO);
    assert(fk.opens[ZZRUN_MODE_NEW] == 0);
    fk.fail_transport_lock = 0;
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_a_lost_request_does_not_create_or_truncate(void)
{
    fixture(0);
    fk.live = 0;
    assert(do_open(3, W | C, "gone.txt") == AAR_E_IO);
    assert(child_of(N_ROOT, "gone.txt") < 0);
    fk.live = 1;
    /* a Ctrl-C an earlier wait consumed stops the destructive step after the
     * READWRITE open of a truncating open: nothing is truncated */
    fk.break_after_open = 1;
    assert(do_open(3, W | T, "top.txt") == AAR_E_IO);
    assert(fk.set_sizes == 0 && fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    assert(fk.live_fhs == 0);
    fk.break_after_open = 0;
    fk.broken = 0;
    /* CREATE|TRUNCATE (MODE_NEWFILE) does not even send its packet */
    fk.broken = 1;
    assert(do_open(3, W | C | T, "top.txt") == AAR_E_IO);
    assert(fk.opens[ZZRUN_MODE_NEW] == 0);
    assert(fk.nodes[child_of(N_ROOT, "top.txt")].size == 3u);
    /* a read-only open needs no such check */
    assert(do_open(3, R, "top.txt") == AAR_OK);
    fk.broken = 0;
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_the_descriptor_table(void)
{
    uint32_t fd;

    fixture(0);
    assert(do_open(2, R, "top.txt") == AAR_E_BADF);
    assert(do_open(3u + ZZRUN_FD_COUNT, R, "top.txt") == AAR_E_BADF);
    for (fd = 3u; fd < 3u + ZZRUN_FD_COUNT; fd++)
        assert(do_open(fd, R, "top.txt") == AAR_OK);
    assert(fk.live_fhs == (int)ZZRUN_FD_COUNT);
    assert(do_open(3, R, "top.txt") == AAR_E_BUSY);
    assert(fk.live_fhs == (int)ZZRUN_FD_COUNT);
    /* a close whose packet was never delivered keeps the slot */
    fk.close_undelivered = 1;
    assert(do_close(4) == AAR_E_IO);
    assert(do_open(4, R, "top.txt") == AAR_E_BUSY);
    fk.close_undelivered = 0;
    assert(do_close(4) == AAR_OK);
    assert(do_open(4, R, "top.txt") == AAR_OK);
    /* open with an unterminated or empty path */
    assert(do_open(3, R, "") == AAR_E_BUSY);
    fixture_free();

    fixture(0);
    assert(do_open(3, R, "") == AAR_E_INVAL);
    {
        struct zzrun_req r = mkreq(AAR_OP_FILE_OPEN);
        struct zzrun_done d;
        uint8_t full[ZZRUN_PATH_BYTES + 1u];

        memset(full, 'a', sizeof(full));
        r.arg[AAR_A_FILE_OPEN_FD] = 3u;
        r.arg[AAR_A_FILE_OPEN_FLAGS] = R;
        r.buf = full;
        r.buf_len = sizeof(full);
        assert(serve_one(&r, &d) == AAR_E_INVAL);
        r.buf = NULL;
        r.buf_len = 0;
        assert(serve_one(&r, &d) == AAR_E_INVAL);
    }
    fixture_free();
}

static void test_list_walks_the_directory(void)
{
    char name[40];
    uint32_t mode;

    fixture(0);
    /* the ROOT itself, by the empty name */
    assert(do_list_open(3, "") == AAR_OK);
    assert(do_list_open(3, "") == AAR_E_BUSY);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "dir") == 0 && mode == AAR_MODE_DIR);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "top.txt") == 0 && mode == AAR_MODE_FILE);
    assert(do_list_next(3, name, 40u, &mode) == AAR_OK);
    assert(strcmp(name, "escape") == 0);
    assert(do_list_next(3, name, 40u, &mode) == AAR_E_NOENT);
    assert(do_list_close(3) == AAR_OK);
    assert(do_list_close(3) == AAR_E_BADF);
    assert(do_list_next(3, name, 40u, &mode) == AAR_E_BADF);

    /* a subdirectory, and a name cut to the room the payload has */
    assert(do_list_open(4, "dir") == AAR_OK);
    assert(do_list_next(4, name, 4u, &mode) == AAR_OK);
    assert(strcmp(name, "hel") == 0 && mode == AAR_MODE_FILE);
    assert(do_list_next(4, name, 1u, &mode) == AAR_E_INVAL);
    assert(do_list_close(4) == AAR_OK);

    /* not a directory, not there */
    assert(do_list_open(3, "top.txt") == AAR_E_IO);
    assert(do_list_open(3, "nothing") == AAR_E_NOENT);
    assert(do_list_open(2, "") == AAR_E_BADF);
    /* files and lists have their own tables */
    assert(do_open(3, R, "top.txt") == AAR_OK);
    assert(do_list_open(3, "") == AAR_OK);
    assert(do_list_close(3) == AAR_OK);
    assert(do_close(3) == AAR_OK);
    fixture_free();
}

static void test_unknown_ops_are_not_implemented(void)
{
    struct zzrun_req r;
    struct zzrun_done d;

    fixture(0);
    r = mkreq(AAR_OP_NONE);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    r = mkreq(0x7fu);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    r = mkreq(AAR_OP_MSG);
    assert(serve_one(&r, &d) == AAR_E_NOSYS);
    fixture_free();
}

static void test_dos_errors_map_to_aar_errors(void)
{
    assert(zzrun_errno_from_io(0) == AAR_OK);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_OBJECT_NOT_FOUND) == AAR_E_NOENT);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_DIR_NOT_FOUND) == AAR_E_NOENT);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_NO_MORE_ENTRIES) == AAR_E_NOENT);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_DISK_FULL) == AAR_E_NOSPC);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_WRITE_PROTECTED) == AAR_E_ACCES);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_READ_PROTECTED) == AAR_E_ACCES);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_DELETE_PROTECTED) == AAR_E_ACCES);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_OBJECT_EXISTS) == AAR_E_EXIST);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_NO_FREE_STORE) == AAR_E_NOMEM);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_OBJECT_IN_USE) == AAR_E_BUSY);
    assert(zzrun_errno_from_io(ZZRUN_IOERR_SEEK_ERROR) == AAR_E_INVAL);
    assert(zzrun_errno_from_io(12345) == AAR_E_IO);
    assert(zzrun_amiga_unix(0u, 0u, 0u) == 0u);
    assert(zzrun_amiga_unix(1u, 2u, 100u) ==
           (uint32_t)ZZRUN_UNIX_DELTA + 86400u + 120u + 2u);
}

/* ---- Console ------------------------------------------------------------ */

struct fcon {
    int open_ok;
    int write_ok;
    int read_ok;
    int allowed;
    int errors;
    int opens;
    int read_starts;
    uint32_t read_max;
    int writes;
    uint8_t written[8192];
    uint32_t written_n;
    uint32_t last_write;
};

static struct fcon fc;

static int c_open(void *ctx)
{
    (void)ctx;
    fc.opens++;
    return fc.open_ok;
}

static int c_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    if (!fc.write_ok)
        return 0;
    assert(bytes > 0 && bytes <= ZZRUN_CONSOLE_WRITE_BYTES);
    memcpy(fc.written + fc.written_n, data, bytes);
    fc.last_write = bytes;
    fc.writes++;
    return 1;
}

static int c_read_start(void *ctx, uint32_t max)
{
    (void)ctx;
    fc.read_starts++;
    fc.read_max = max;
    return fc.read_ok;
}

static int c_read_allowed(void *ctx)
{
    (void)ctx;
    return fc.allowed;
}

static void c_output_error(void *ctx)
{
    (void)ctx;
    fc.errors++;
}

static void console_fixture(void)
{
    struct zzrun_console_ops ops;

    memset(&fc, 0, sizeof(fc));
    fc.open_ok = fc.write_ok = fc.read_ok = fc.allowed = 1;
    memset(&ops, 0, sizeof(ops));
    ops.open = c_open;
    ops.write_start = c_write_start;
    ops.read_start = c_read_start;
    ops.read_allowed = c_read_allowed;
    ops.output_error = c_output_error;
    zzrun_serve_init(&serve, &sink);
    zzrun_console_init(&serve.console, &ops, &serve.sink);
    sunk_n = 0;
    next_id = 200u;
}

/* The write finished: what the DOS packet reported for the bytes it was
 * given (the whole span by default). */
static void write_finishes(int32_t written)
{
    fc.written_n += fc.last_write;
    zzrun_console_write_done(&serve.console, written);
}

static void test_console_write_completes_when_the_dos_write_does(void)
{
    uint8_t text[3000];
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct zzrun_done d;
    uint32_t i;

    console_fixture();
    for (i = 0; i < sizeof(text); i++)
        text[i] = (uint8_t)(i % 251u);
    r.buf = text;
    r.buf_len = sizeof(text);
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    /* one packet is in flight, nothing is completed */
    assert(fc.writes == 1 && fc.last_write == ZZRUN_CONSOLE_WRITE_BYTES);
    assert(sunk_n == 0 && zzrun_console_output_pending(&serve.console));
    /* a second request meanwhile is busy: the firmware keeps one */
    {
        struct zzrun_req r2 = mkreq(AAR_OP_CONSOLE_WRITE);
        struct zzrun_done d2;

        r2.buf = text;
        r2.buf_len = 4u;
        assert(zzrun_serve_request(&serve, &r2, &d2) == ZZRUN_SERVE_DONE);
        assert(d2.result == AAR_E_BUSY);
    }
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 2 && sunk_n == 0);
    write_finishes((int32_t)fc.last_write);
    assert(fc.writes == 3 && fc.last_write == 3000u - 2048u && sunk_n == 0);
    write_finishes((int32_t)fc.last_write);
    assert(sunk_n == 1 && sunk[0].req.id == r.id);
    assert(sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 3000u);
    assert(fc.written_n == 3000u && memcmp(fc.written, text, 3000u) == 0);
    assert(!zzrun_console_output_pending(&serve.console));
    assert(fc.errors == 0 && fc.opens >= 1);
}

static void test_console_write_edges(void)
{
    uint8_t text[8] = "abcdefgh";
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct zzrun_done d;

    console_fixture();
    /* nothing to write completes at once */
    r.buf = text;
    r.buf_len = 0;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_OK && d.out[AAR_R_CONSOLE_COUNT] == 0u);
    /* bytes with no payload are refused */
    r.buf = NULL;
    r.buf_len = 8u;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_E_INVAL);
    assert(fc.writes == 0);

    /* the console cannot be opened */
    console_fixture();
    fc.open_ok = 0;
    r.buf = text;
    r.buf_len = 8u;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.errors == 1 && fc.writes == 0);
    /* and stays failed */
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_E_IO && fc.errors == 1);

    /* the packet cannot start */
    console_fixture();
    fc.write_ok = 0;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(fc.errors == 1);
}

static void test_a_short_write_fails_the_output_after_its_partial_advance(void)
{
    uint8_t text[100];
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct zzrun_done d;

    console_fixture();
    memset(text, 'x', sizeof(text));
    r.buf = text;
    r.buf_len = sizeof(text);
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    zzrun_console_write_done(&serve.console, 40);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 40u);
    assert(fc.errors == 1);
    assert(fc.writes == 1);

    /* a failed write (0 or negative) advances nothing */
    console_fixture();
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    zzrun_console_write_done(&serve.console, -1);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u && fc.errors == 1);
}

static void test_launcher_text_goes_before_app_text(void)
{
    uint8_t text[4] = "app!";
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_WRITE);
    struct zzrun_done d;

    console_fixture();
    zzrun_console_launcher_status(&serve.console, "hello");
    assert(zzrun_console_output_pending(&serve.console));
    zzrun_console_pump(&serve.console);
    assert(fc.writes == 1 && memcmp(fc.written, "hello\n", 6u) == 0);
    /* the app asks while the launcher packet is in flight */
    r.buf = text;
    r.buf_len = 4u;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(fc.writes == 1 && sunk_n == 0);
    zzrun_console_launcher_status(&serve.console, "again");
    write_finishes(6);
    /* a launcher message queued before the app bytes goes first */
    assert(fc.writes == 2 && memcmp(fc.written + 6, "again\n", 6u) == 0);
    write_finishes(6);
    assert(fc.writes == 3 && memcmp(fc.written + 12, "app!", 4u) == 0);
    write_finishes(4);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 4u);
    assert(!zzrun_console_output_pending(&serve.console));

    /* a partial launcher write resumes where it left off */
    console_fixture();
    zzrun_console_launcher_status(&serve.console, "abcdef");
    zzrun_console_pump(&serve.console);
    fc.written_n += 0;
    zzrun_console_write_done(&serve.console, 3);
    assert(fc.errors == 1);
}

static void test_launcher_output_keeps_status_room_for_non_diagnostics(void)
{
    uint8_t line[1000];
    uint32_t i;

    console_fixture();
    memset(line, 'd', sizeof(line));
    for (i = 0; i < 20u; i++)
        zzrun_console_launcher_put(&serve.console, line, sizeof(line), 1);
    /* diagnostics stop at the limit and are counted; status text still fits */
    assert(serve.console.launcher_used <= ZZRUN_LAUNCHER_OUTPUT_BYTES -
                                          ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES);
    assert(serve.console.launcher_dropped > 0u);
    {
        uint32_t before = serve.console.launcher_used;

        zzrun_console_launcher_status(&serve.console, "still fits");
        assert(serve.console.launcher_used == before + 11u);
    }
}

static void test_log_records_become_lines(void)
{
    static const uint8_t plain[] = "boot ok";
    static const uint8_t with_nl[] = "line\n";
    uint8_t big[1000];
    uint32_t i;

    console_fixture();
    /* a record without a newline gets one; one with it is left alone */
    zzrun_console_launcher_log(&serve.console, plain, 7u);
    assert(serve.console.launcher_used == 8u);
    zzrun_console_launcher_log(&serve.console, with_nl, 5u);
    assert(serve.console.launcher_used == 13u);
    zzrun_console_launcher_log(&serve.console, plain, 0u); /* empty: a blank line */
    assert(serve.console.launcher_used == 14u);
    zzrun_console_pump(&serve.console);
    assert(fc.writes == 1 && fc.last_write == 14u);
    assert(memcmp(fc.written, "boot ok\nline\n\n", 14u) == 0);
    write_finishes(14);
    assert(!zzrun_console_output_pending(&serve.console));

    /* logs are diagnostics: they stop at the limit and are counted */
    console_fixture();
    memset(big, 'l', sizeof(big));
    for (i = 0; i < 20u; i++)
        zzrun_console_launcher_log(&serve.console, big, sizeof(big));
    assert(serve.console.launcher_dropped > 0u);
    assert(serve.console.launcher_used <= ZZRUN_LAUNCHER_OUTPUT_BYTES -
                                          ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES);
}

static void test_console_read_waits_for_demand_and_returns_what_was_read(void)
{
    uint8_t buf[512];
    uint8_t line[] = "typed\n";
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct zzrun_done d;

    console_fixture();
    fc.allowed = 0;
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    /* the app is not RUNNING: no DOS read is started */
    assert(fc.read_starts == 0);
    zzrun_console_pump(&serve.console);
    assert(fc.read_starts == 0);
    fc.allowed = 1;
    zzrun_console_pump(&serve.console);
    assert(fc.read_starts == 1 && fc.read_max == ZZRUN_CONSOLE_READ_BYTES);
    zzrun_console_pump(&serve.console);
    assert(fc.read_starts == 1); /* one read per request */
    /* a second outstanding read is refused */
    {
        struct zzrun_req r2 = mkreq(AAR_OP_CONSOLE_READ);
        struct zzrun_done d2;

        r2.buf = buf;
        r2.buf_len = sizeof(buf);
        assert(zzrun_serve_request(&serve, &r2, &d2) == ZZRUN_SERVE_DONE);
        assert(d2.result == AAR_E_BUSY);
    }
    zzrun_console_read_done(&serve.console, 6, line);
    assert(sunk_n == 1 && sunk[0].req.id == r.id);
    assert(sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 6u);
    assert(memcmp(buf, "typed\n", 6u) == 0);
    /* a room smaller than a line reads at most the room */
    r = mkreq(AAR_OP_CONSOLE_READ);
    r.buf = buf;
    r.buf_len = 3u;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(fc.read_starts == 2 && fc.read_max == 3u);
}

static void test_console_read_end_of_input_is_sticky(void)
{
    uint8_t buf[16];
    struct zzrun_req r = mkreq(AAR_OP_CONSOLE_READ);
    struct zzrun_done d;

    console_fixture();
    r.buf = buf;
    r.buf_len = sizeof(buf);
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    zzrun_console_read_done(&serve.console, 0, NULL);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    /* every later read answers 0 without touching DOS */
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_OK && d.out[AAR_R_CONSOLE_COUNT] == 0u);
    assert(fc.read_starts == 1);

    /* a failed read is the end of the input too */
    console_fixture();
    fc.read_ok = 0;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_OK &&
           sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.out[AAR_R_CONSOLE_COUNT] == 0u && fc.read_starts == 1);

    /* a negative DOS result is the end too; a console that will not open
     * fails the request */
    console_fixture();
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    zzrun_console_read_done(&serve.console, -1, NULL);
    assert(sunk[0].done.out[AAR_R_CONSOLE_COUNT] == 0u);
    console_fixture();
    fc.open_ok = 0;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    assert(sunk_n == 1 && sunk[0].done.result == AAR_E_IO);

    /* no payload, no room */
    console_fixture();
    r.buf = NULL;
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_E_INVAL);
}

/* ---- Display ------------------------------------------------------------ */

struct fdisp {
    int open_calls;
    int changes;
    int change_ok;
    int last_to_back;
    int pans;
    uint32_t last_page;
    int tofs;
    int loads;
    uint32_t table[2 + 256 * 3];
    int closes;
    int have_two;
    int have_pan;
    uint32_t pan_count;
};

static struct fdisp fd_;

static void d_open(void *ctx, const struct zzrun_req *req, struct zzrun_done *done)
{
    (void)ctx;
    fd_.open_calls++;
    /* 640x480 at 16 bit: pitch 1280, tokens at 0x00400000 */
    zzrun_display_opened(&serve.display, req, 1280u, 0x00400000u,
                         fd_.have_two ? 0x00600000u :
                         (fd_.have_pan ? 0x00400000u + 1280u * 480u : 0u),
                         fd_.pan_count == 3u ? 0x00400000u + 2u * 1280u * 480u : 0u,
                         fd_.have_two, fd_.have_pan, fd_.pan_count, done);
}

static int d_change(void *ctx, int to_back)
{
    (void)ctx;
    fd_.changes++;
    fd_.last_to_back = to_back;
    return fd_.change_ok;
}

static void d_pan(void *ctx, uint32_t page)
{
    (void)ctx;
    fd_.pans++;
    fd_.last_page = page;
}

static void d_tof(void *ctx)
{
    (void)ctx;
    fd_.tofs++;
}

static void d_load(void *ctx, const uint32_t *table)
{
    uint32_t count = table[0] >> 16;

    (void)ctx;
    fd_.loads++;
    memcpy(fd_.table, table, (2u + count * 3u) * 4u);
}

static void d_close(void *ctx)
{
    (void)ctx;
    fd_.closes++;
}

static void display_fixture(int two, int pan, uint32_t pan_count)
{
    struct zzrun_display_ops ops;

    memset(&fd_, 0, sizeof(fd_));
    fd_.change_ok = 1;
    fd_.have_two = two;
    fd_.have_pan = pan;
    fd_.pan_count = pan_count;
    memset(&ops, 0, sizeof(ops));
    ops.open = d_open;
    ops.change_buffer = d_change;
    ops.pan = d_pan;
    ops.wait_tof = d_tof;
    ops.load_rgb32 = d_load;
    ops.close = d_close;
    zzrun_serve_init(&serve, &sink);
    zzrun_display_init(&serve.display, &ops, &serve.sink);
    sunk_n = 0;
    next_id = 300u;
}

static struct zzrun_done display_open(uint32_t w, uint32_t h, uint32_t fmt,
                                      uint32_t flags, int32_t expect)
{
    struct zzrun_req r = mkreq(AAR_OP_DISPLAY_OPEN);
    struct zzrun_done d;

    r.arg[AAR_A_DISPLAY_OPEN_W] = w;
    r.arg[AAR_A_DISPLAY_OPEN_H] = h;
    r.arg[AAR_A_DISPLAY_OPEN_FORMAT] = fmt;
    r.arg[AAR_A_DISPLAY_OPEN_FLAGS] = flags;
    assert(serve_one(&r, &d) == expect);
    return d;
}

static struct zzrun_req flip_req(void)
{
    struct zzrun_req r = mkreq(AAR_OP_DISPLAY_FLIP);

    r.arg[AAR_A_DISPLAY_ID] = ZZRUN_DISPLAY_ID;
    return r;
}

static void test_display_open_validates_and_reports_what_was_made(void)
{
    struct zzrun_done d;

    display_fixture(0, 1, 3u);
    display_open(0u, 480u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 0u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(4097u, 480u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 4097u, AAR_FMT_RGB565, 0u, AAR_E_INVAL);
    display_open(640u, 480u, 0u, 0u, AAR_E_INVAL);
    display_open(640u, 480u, 4u, 0u, AAR_E_INVAL);
    assert(fd_.open_calls == 0);

    d = display_open(640u, 480u, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, AAR_OK);
    assert(fd_.open_calls == 1);
    assert(d.out[AAR_R_DISPLAY_OPEN_ID] == ZZRUN_DISPLAY_ID);
    assert(d.out[AAR_R_DISPLAY_OPEN_PITCH] == 1280u);
    assert(d.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    assert(d.out[AAR_R_DISPLAY_OPEN_FRONT] == 0x00400000u);
    assert(d.out[AAR_R_DISPLAY_OPEN_BACK] == 0x00400000u + 1280u * 480u);
    assert(d.out[AAR_R_DISPLAY_OPEN_SPARE] == 0x00400000u + 2u * 1280u * 480u);
    /* async and CLUT8 are always granted, latest only when asked for */
    assert(d.flags == (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8));
    /* a second open while one is open is refused */
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_E_BUSY);
    assert(fd_.open_calls == 1);

    assert(zzrun_display_layout(AAR_FMT_BGRA8888) == AAR_LAYOUT_BGRA8888);
    assert(zzrun_display_layout(AAR_FMT_CLUT8) == AAR_LAYOUT_CLUT8);
    assert(zzrun_display_layout(AAR_FMT_RGB565) == AAR_LAYOUT_RGB565_BE);
    assert(zzrun_display_depth(AAR_FMT_BGRA8888) == 32u);
    assert(zzrun_display_depth(AAR_FMT_CLUT8) == 8u);
    assert(zzrun_display_depth(AAR_FMT_RGB565) == 16u);
    assert(zzrun_display_caps(AAR_DISPLAY_LATEST) ==
           (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 | AAR_DISPCAP_LATEST));
    assert(zzrun_display_caps(AAR_DISPLAY_VSYNC) ==
           (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8));

    display_fixture(0, 1, 2u);
    d = display_open(640u, 480u, AAR_FMT_CLUT8,
                     AAR_DISPLAY_LATEST | AAR_DISPLAY_VSYNC, AAR_OK);
    assert(d.flags == (AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 |
                       AAR_DISPCAP_LATEST));
    assert(d.out[AAR_R_DISPLAY_OPEN_LAYOUT] == AAR_LAYOUT_CLUT8);
    assert(d.out[AAR_R_DISPLAY_OPEN_SPARE] == 0u);
}

static void test_pan_flips_rotate_the_pages_and_rely_on_the_card_vblank(void)
{
    struct zzrun_req r;
    struct zzrun_done d;
    const uint32_t page = 1280u * 480u;
    const uint32_t base = 0x00400000u;

    display_fixture(0, 1, 3u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK);
    /* pan to page 1: it is the front, page 2 is the next to draw into */
    assert(fd_.pans == 1 && fd_.last_page == 1u && fd_.tofs == 0);
    assert(d.flags == ZZAPP_FLIPF_SUBMITTED);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base + page);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + 2u * page);
    assert(d.out[AAR_R_DISPLAY_FLIP_PITCH] == 1280u);
    assert(d.out[AAR_R_DISPLAY_FLIP_LAYOUT] == AAR_LAYOUT_RGB565_BE);
    assert(d.out[AAR_R_DISPLAY_FLIP_DROPPED] == 0u);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 2u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base + 2u * page);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == base);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + page);
    assert(fd_.pans == 3 && sunk_n == 0);

    /* double buffering by panning: two pages */
    display_fixture(0, 1, 2u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 1u);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.last_page == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_BACK] == base + page);
}

static void test_screen_buffer_flips_complete_on_the_safe_message(void)
{
    struct zzrun_req r1;
    struct zzrun_req r2;
    struct zzrun_req r3;
    struct zzrun_done d;

    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r1 = flip_req();
    assert(zzrun_serve_request(&serve, &r1, &d) == ZZRUN_SERVE_DEFERRED);
    assert(fd_.changes == 1 && fd_.last_to_back == 1 && sunk_n == 0);
    /* a second flip meanwhile waits its turn, a third is refused */
    r2 = flip_req();
    assert(zzrun_serve_request(&serve, &r2, &d) == ZZRUN_SERVE_DEFERRED);
    assert(fd_.changes == 1);
    r3 = flip_req();
    assert(zzrun_serve_request(&serve, &r3, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_E_BUSY);

    zzrun_display_flip_safe(&serve.display);
    /* the first completes with the buffers as they were left (back showing);
     * the second flip started back to the front buffer */
    assert(sunk_n == 1 && sunk[0].req.id == r1.id && sunk[0].done.result == AAR_OK);
    assert(sunk[0].done.flags == 0u);
    assert(fd_.changes == 2 && fd_.last_to_back == 0);
    zzrun_display_flip_safe(&serve.display);
    assert(sunk_n == 2 && sunk[1].req.id == r2.id && sunk[1].done.result == AAR_OK);
    /* a stray SafeMessage completes nothing */
    zzrun_display_flip_safe(&serve.display);
    assert(sunk_n == 2);

    /* ChangeScreenBuffer fails */
    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    fd_.change_ok = 0;
    r1 = flip_req();
    assert(zzrun_serve_request(&serve, &r1, &d) == ZZRUN_SERVE_DONE);
    assert(d.result == AAR_E_IO);
    /* the failed flip left nothing waiting for a message */
    zzrun_display_flip_safe(&serve.display);
    assert(sunk_n == 0);

    /* a queued flip that cannot start completes with the error */
    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r1 = flip_req();
    r2 = flip_req();
    assert(zzrun_serve_request(&serve, &r1, &d) == ZZRUN_SERVE_DEFERRED);
    assert(zzrun_serve_request(&serve, &r2, &d) == ZZRUN_SERVE_DEFERRED);
    fd_.change_ok = 0;
    zzrun_display_flip_safe(&serve.display);
    assert(sunk_n == 2 && sunk[0].done.result == AAR_OK &&
           sunk[1].done.result == AAR_E_IO);
}

static void test_a_single_buffer_flip_waits_for_the_frame_only_when_asked(void)
{
    struct zzrun_req r;
    struct zzrun_done d;

    display_fixture(0, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.tofs == 0 && fd_.pans == 0 && fd_.changes == 0);
    assert(d.flags == 0u);
    assert(d.out[AAR_R_DISPLAY_FLIP_FRONT] == 0x00400000u &&
           d.out[AAR_R_DISPLAY_FLIP_BACK] == 0x00400000u);

    display_fixture(0, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, AAR_OK);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_OK && fd_.tofs == 1);
}

static void test_flips_name_the_open_display(void)
{
    struct zzrun_req r;
    struct zzrun_done d;

    display_fixture(0, 1, 2u);
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_E_INVAL); /* nothing open */
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = flip_req();
    r.arg[AAR_A_DISPLAY_ID] = 9u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fd_.pans == 0);
}

static struct zzrun_req palette_req(uint32_t first, uint32_t count,
                                    uint8_t *words, uint32_t bytes)
{
    struct zzrun_req r = mkreq(AAR_OP_DISPLAY_PALETTE);

    r.arg[AAR_A_DISPLAY_ID] = ZZRUN_DISPLAY_ID;
    r.arg[AAR_A_DISPLAY_PALETTE_FIRST] = first;
    r.arg[AAR_A_DISPLAY_PALETTE_COUNT] = count;
    r.buf = words;
    r.buf_len = bytes;
    return r;
}

static void test_palette_becomes_a_loadrgb32_table(void)
{
    uint8_t words[16 * 4];
    struct zzrun_req r;
    struct zzrun_done d;
    uint32_t i;

    display_fixture(0, 1, 2u);
    display_open(320u, 200u, AAR_FMT_CLUT8, 0u, AAR_OK);
    for (i = 0; i < 16u; i++)
        zzrun_put_be32(words + i * 4u, (i << 16) | ((i + 1u) << 8) | (i + 2u));
    r = palette_req(10u, 16u, words, sizeof(words));
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.loads == 1);
    assert(fd_.table[0] == ((16u << 16) | 10u));
    for (i = 0; i < 16u; i++) {
        assert(fd_.table[1 + i * 3u] == i * 0x01010101u);
        assert(fd_.table[2 + i * 3u] == (i + 1u) * 0x01010101u);
        assert(fd_.table[3 + i * 3u] == (i + 2u) * 0x01010101u);
    }
    assert(fd_.table[1 + 16u * 3u] == 0u);
    /* the high byte of a word is ignored */
    zzrun_put_be32(words, 0xff112233u);
    r = palette_req(0u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fd_.table[1] == 0x11u * 0x01010101u &&
           fd_.table[2] == 0x22u * 0x01010101u &&
           fd_.table[3] == 0x33u * 0x01010101u);
    /* the last colour fits, one more does not */
    r = palette_req(255u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_OK);
    r = palette_req(255u, 2u, words, 8u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(256u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 0u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    /* fewer bytes than colours */
    r = palette_req(0u, 2u, words, 7u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 2u, NULL, 0u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = palette_req(0u, 1u, words, 4u);
    r.arg[AAR_A_DISPLAY_ID] = 5u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fd_.loads == 3);

    /* a full table fits the buffer the handler builds */
    {
        uint8_t all[256 * 4];

        memset(all, 0xff, sizeof(all));
        r = palette_req(0u, 256u, all, sizeof(all));
        assert(serve_one(&r, &d) == AAR_OK);
        assert(fd_.table[0] == (256u << 16) &&
               fd_.table[1 + 255u * 3u] == 0xffffffffu &&
               fd_.table[1 + 256u * 3u] == 0u);
    }

    /* only a CLUT8 screen has a palette */
    display_fixture(0, 1, 2u);
    display_open(320u, 200u, AAR_FMT_RGB565, 0u, AAR_OK);
    r = palette_req(0u, 1u, words, 4u);
    assert(serve_one(&r, &d) == AAR_E_INVAL && fd_.loads == 0);
}

static void test_display_close_ends_the_screen_and_pending_flips(void)
{
    struct zzrun_req r;
    struct zzrun_req c;
    struct zzrun_done d;

    display_fixture(1, 0, 0u);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    c = mkreq(AAR_OP_DISPLAY_CLOSE);
    c.arg[AAR_A_DISPLAY_ID] = 7u;
    assert(serve_one(&c, &d) == AAR_E_INVAL && fd_.closes == 0);
    r = flip_req();
    assert(zzrun_serve_request(&serve, &r, &d) == ZZRUN_SERVE_DEFERRED);
    c.arg[AAR_A_DISPLAY_ID] = ZZRUN_DISPLAY_ID;
    assert(serve_one(&c, &d) == AAR_OK && fd_.closes == 1);
    /* the flip that was waiting for its SafeMessage is answered */
    assert(sunk_n == 1 && sunk[0].req.id == r.id &&
           sunk[0].done.result == AAR_E_IO);
    /* closed: a flip names nothing, and the screen can open again */
    r = flip_req();
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(serve_one(&c, &d) == AAR_E_INVAL && fd_.closes == 1);
    display_open(640u, 480u, AAR_FMT_RGB565, 0u, AAR_OK);
    assert(fd_.open_calls == 2);
}

/* ---- Audio -------------------------------------------------------------- */

struct faud {
    int opens;
    int open_ok;
    int closes;
    uint32_t rate, channels, token, bytes, frames;
};

static struct faud fa;

static int a_open(void *ctx, uint32_t rate, uint32_t channels, uint32_t token,
                  uint32_t bytes, uint32_t frames)
{
    (void)ctx;
    fa.opens++;
    fa.rate = rate;
    fa.channels = channels;
    fa.token = token;
    fa.bytes = bytes;
    fa.frames = frames;
    return fa.open_ok;
}

static void a_close(void *ctx)
{
    (void)ctx;
    fa.closes++;
}

static void audio_fixture(void)
{
    struct zzrun_audio_ops ops;

    memset(&fa, 0, sizeof(fa));
    fa.open_ok = 1;
    ops.ctx = NULL;
    ops.open = a_open;
    ops.close = a_close;
    zzrun_serve_init(&serve, &sink);
    zzrun_audio_init(&serve.audio, &ops);
    next_id = 400u;
}

static struct zzrun_req audio_req(uint32_t rate, uint32_t channels,
                                  uint32_t route, uint32_t token,
                                  uint32_t bytes)
{
    struct zzrun_req r = mkreq(AAR_OP_AUDIO_OPEN);

    r.arg[AAR_A_AUDIO_OPEN_RATE] = rate;
    r.arg[AAR_A_AUDIO_OPEN_CHANNELS] = channels;
    r.arg[AAR_A_AUDIO_OPEN_ROUTE] = route;
    r.buf_token = token;
    r.buf_len = bytes;
    return r;
}

static void test_audio_open_binds_the_runtimes_ring(void)
{
    struct zzrun_req r;
    struct zzrun_done d;

    audio_fixture();
    /* 0x80 header + 8192 frames of 16 bit stereo */
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x00120000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_OK);
    assert(fa.opens == 1 && fa.rate == 44100u && fa.channels == 2u &&
           fa.token == 0x00120000u && fa.bytes == 0x8080u && fa.frames == 8192u);
    assert(d.out[AAR_R_AUDIO_OPEN_RATE] == 44100u);
    assert(d.out[AAR_R_AUDIO_OPEN_CHANNELS] == 2u);
    assert(d.out[AAR_R_AUDIO_OPEN_RING] == 0x00120000u);
    assert(d.out[AAR_R_AUDIO_OPEN_RING_BYTES] == 0x8080u);
    assert(d.out[AAR_R_AUDIO_OPEN_FRAMES] == 8192u);
    /* a block rounded up to 40 KiB still holds 8192 frames, no more */
    r = audio_req(22050u, 2u, AAR_AUDIO_ROUTE_AHI, 0x00130000u, 0xa000u);
    assert(serve_one(&r, &d) == AAR_OK && fa.frames == 8192u);
    assert(fa.closes == 1); /* reopening closes the old stream first */
    assert(d.out[AAR_R_AUDIO_OPEN_FRAMES] == 8192u);

    r = mkreq(AAR_OP_AUDIO_CLOSE);
    assert(serve_one(&r, &d) == AAR_OK && fa.closes == 2);
    assert(serve_one(&r, &d) == AAR_OK && fa.closes == 2); /* idempotent */
}

static void test_audio_open_refusals(void)
{
    struct zzrun_req r;
    struct zzrun_done d;

    audio_fixture();
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_PLATFORM, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(0u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 1u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x80u);
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    r.buf_off = 16u;
    assert(serve_one(&r, &d) == AAR_E_INVAL);
    assert(fa.opens == 0);

    /* AHI cannot be opened */
    fa.open_ok = 0;
    r = audio_req(44100u, 2u, AAR_AUDIO_ROUTE_AHI, 0x1000u, 0x8080u);
    assert(serve_one(&r, &d) == AAR_E_IO);
    assert(serve.audio.open_failed && !serve.audio.open);
}

static void test_audio_chunk_is_a_fiftieth_of_a_second_at_most(void)
{
    /* 44100 Hz: 882 frames per 1/50 s */
    assert(zzrun_audio_chunk_frames(5000u, 44100u, 2u, 8192u) == 882u);
    assert(zzrun_audio_chunk_frames(100u, 44100u, 2u, 8192u) == 100u);
    assert(zzrun_audio_chunk_frames(0u, 44100u, 2u, 8192u) == 0u);
    /* 48 kHz: 960 frames; the buffer holds 2048 frames of stereo */
    assert(zzrun_audio_chunk_frames(5000u, 48000u, 2u, 8192u) == 960u);
    assert(zzrun_audio_chunk_frames(5000u, 192000u, 2u, 8192u) == 2048u);
    assert(zzrun_audio_chunk_frames(5000u, 44100u, 0u, 8192u) == 0u);
}

/* ---- Events ------------------------------------------------------------- */

struct fev {
    int full;
    int posted;
    aar_event last;
    int calls;
};

static struct fev fe;

static int e_post(void *ctx, const aar_event *ev)
{
    (void)ctx;
    fe.calls++;
    if (fe.full)
        return AAR_E_BUSY;
    fe.posted++;
    fe.last = *ev;
    return AAR_OK;
}

static void test_a_stop_that_found_the_queue_full_is_retried(void)
{
    struct zzrun_events e;
    struct zzrun_events_ops ops;

    ops.ctx = NULL;
    ops.post = e_post;
    memset(&fe, 0, sizeof(fe));
    zzrun_events_init(&e, &ops);

    /* an ordinary event goes through, with its fields */
    assert(zzrun_events_push(&e, AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0x8u, 1234u) ==
           AAR_OK);
    assert(fe.posted == 1 && fe.last.type == AAR_EV_KEY_DOWN &&
           fe.last.code == 0x45u && fe.last.buttons == 0x8u &&
           fe.last.time_ms == 1234u);
    assert(zzrun_events_push(&e, AAR_EV_MOUSE, 0u, -3, 5, 1u, 5u) == AAR_OK);
    assert(fe.last.dx == -3 && fe.last.dy == 5);

    /* a full queue drops an ordinary event (the module counts it) ... */
    fe.full = 1;
    assert(zzrun_events_push(&e, AAR_EV_KEY_UP, 0x45u, 0, 0, 0u, 1u) ==
           AAR_E_BUSY);
    assert(!zzrun_events_pending(&e));
    /* ... but a stop is kept and retried until it is delivered */
    assert(zzrun_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 77u) == AAR_OK);
    assert(zzrun_events_pending(&e));
    assert(zzrun_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 78u) == AAR_OK);
    zzrun_events_flush(&e);
    assert(zzrun_events_pending(&e));
    fe.full = 0;
    fe.posted = 0;
    zzrun_events_flush(&e);
    assert(!zzrun_events_pending(&e) && fe.posted == 1);
    assert(fe.last.type == AAR_EV_STOP && fe.last.time_ms == 77u);
    zzrun_events_flush(&e);
    assert(fe.posted == 1);
    /* a stop that gets through at once is posted once */
    fe.posted = 0;
    assert(zzrun_events_push(&e, AAR_EV_STOP, 0u, 0, 0, 0u, 1u) == AAR_OK);
    assert(fe.posted == 1 && !zzrun_events_pending(&e));
}

int main(void)
{
    test_dos_errors_map_to_aar_errors();
    test_read_in_chunks_and_past_the_end();
    test_read_stops_at_a_break_with_the_bytes_so_far();
    test_write_seek_and_stat();
    test_each_open_plan_acts_on_the_file_it_should();
    test_exclusive_create_refuses_an_existing_file();
    test_root_confinement();
    test_transport_failures_are_io_errors_not_dos_results();
    test_a_lost_request_does_not_create_or_truncate();
    test_the_descriptor_table();
    test_list_walks_the_directory();
    test_unknown_ops_are_not_implemented();
    test_console_write_completes_when_the_dos_write_does();
    test_console_write_edges();
    test_a_short_write_fails_the_output_after_its_partial_advance();
    test_launcher_text_goes_before_app_text();
    test_launcher_output_keeps_status_room_for_non_diagnostics();
    test_log_records_become_lines();
    test_console_read_waits_for_demand_and_returns_what_was_read();
    test_console_read_end_of_input_is_sticky();
    test_display_open_validates_and_reports_what_was_made();
    test_pan_flips_rotate_the_pages_and_rely_on_the_card_vblank();
    test_screen_buffer_flips_complete_on_the_safe_message();
    test_a_single_buffer_flip_waits_for_the_frame_only_when_asked();
    test_flips_name_the_open_display();
    test_palette_becomes_a_loadrgb32_table();
    test_display_close_ends_the_screen_and_pending_flips();
    test_audio_open_binds_the_runtimes_ring();
    test_audio_open_refusals();
    test_audio_chunk_is_a_fiftieth_of_a_second_at_most();
    test_a_stop_that_found_the_queue_full_is_retried();
    puts("zzrun_serve_test: ok");
    return 0;
}
