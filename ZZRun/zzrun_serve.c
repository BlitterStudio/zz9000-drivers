/* Service handlers: compiled by host tests and included by ZZRun.c because
 * the Amiga build intentionally has one translation unit. */
#include <stdint.h>
#include <string.h>

#include "zzrun_serve.h"

int32_t zzrun_errno_from_io(int32_t io_error)
{
    switch (io_error) {
    case 0:
        return AAR_OK;
    case ZZRUN_IOERR_OBJECT_NOT_FOUND:
    case ZZRUN_IOERR_DIR_NOT_FOUND:
    case ZZRUN_IOERR_DEVICE_NOT_MOUNTED:
    case ZZRUN_IOERR_NO_MORE_ENTRIES:
        return AAR_E_NOENT;
    case ZZRUN_IOERR_DISK_FULL:
        return AAR_E_NOSPC;
    case ZZRUN_IOERR_WRITE_PROTECTED:
    case ZZRUN_IOERR_READ_PROTECTED:
    case ZZRUN_IOERR_DELETE_PROTECTED:
        return AAR_E_ACCES;
    case ZZRUN_IOERR_OBJECT_EXISTS:
        return AAR_E_EXIST;
    case ZZRUN_IOERR_NO_FREE_STORE:
        return AAR_E_NOMEM;
    case ZZRUN_IOERR_OBJECT_IN_USE:
        return AAR_E_BUSY;
    case ZZRUN_IOERR_SEEK_ERROR:
        return AAR_E_INVAL;
    default:
        return AAR_E_IO;
    }
}

uint32_t zzrun_amiga_unix(uint32_t days, uint32_t minutes, uint32_t ticks)
{
    if (days == 0 && minutes == 0 && ticks == 0)
        return 0;
    return (uint32_t)ZZRUN_UNIX_DELTA + days * 86400u + minutes * 60u +
           ticks / 50u;
}

static void done_clear(struct zzrun_done *done)
{
    memset(done, 0, sizeof(*done));
}

static void done_result(struct zzrun_done *done, int32_t result)
{
    done_clear(done);
    done->result = result;
}

/* ---- Files -------------------------------------------------------------- */

void zzrun_files_init(struct zzrun_files *files, const struct zzrun_dos *dos,
                      uintptr_t root, int abs_paths)
{
    memset(files, 0, sizeof(*files));
    files->dos = dos;
    files->root = root;
    files->abs_paths = abs_paths;
}

void zzrun_files_close_all(struct zzrun_files *f)
{
    unsigned i;

    for (i = 0; i < ZZRUN_FD_COUNT; i++) {
        if (f->fh[i]) {
            f->dos->fh_end_or_close(f->dos->ctx, f->fh[i]);
            f->fh[i] = 0;
        }
        if (f->list[i]) {
            f->dos->list_close(f->dos->ctx, f->list[i]);
            f->list[i] = 0;
        }
    }
}

static int slot_index(int32_t id, int32_t *err)
{
    if (id < AAR_FD_BASE || id >= AAR_FD_BASE + (int32_t)ZZRUN_FD_COUNT) {
        *err = AAR_E_BADF;
        return -1;
    }
    return (int)(id - AAR_FD_BASE);
}

/* The path of a request: NUL terminated inside the payload. */
static int path_from_req(const struct zzrun_req *req, char *out, int32_t *err)
{
    uint32_t i;

    if (!req->buf || req->buf_len == 0) {
        *err = AAR_E_INVAL;
        return 0;
    }
    for (i = 0; i < ZZRUN_PATH_BYTES && i < req->buf_len; i++) {
        out[i] = (char)req->buf[i];
        if (out[i] == 0)
            return 1;
    }
    *err = AAR_E_INVAL;
    return 0;
}

static int under_root(const struct zzrun_files *f, uintptr_t lock)
{
    const struct zzrun_dos *d = f->dos;
    uintptr_t cur = d->dup_lock(d->ctx, lock);

    while (cur) {
        uintptr_t parent;

        if (d->same_lock(d->ctx, cur, f->root)) {
            d->unlock(d->ctx, cur);
            return 1;
        }
        parent = d->parent_dir(d->ctx, cur);
        d->unlock(d->ctx, cur);
        cur = parent;
    }
    return 0;
}

static int step_parent(const struct zzrun_files *f, uintptr_t *cur,
                       int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    uintptr_t parent = d->parent_dir(d->ctx, *cur);

    if (!parent || !under_root(f, parent)) {
        if (parent)
            d->unlock(d->ctx, parent);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, *cur);
    *cur = parent;
    return 1;
}

static int enter_dir(const struct zzrun_files *f, uintptr_t *cur,
                     const char *name, int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t child = d->lock_rel(d->ctx, *cur, name, &transport_ok, &io);

    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!child) {
        *err = io == ZZRUN_IOERR_OBJECT_NOT_FOUND ||
               io == ZZRUN_IOERR_DIR_NOT_FOUND ? AAR_E_NOENT : AAR_E_IO;
        return 0;
    }
    if (!under_root(f, child)) {
        d->unlock(d->ctx, child);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, *cur);
    *cur = child;
    return 1;
}

/* A slash after a name separates the next component. A slash with no name (a
 * leading slash, or two slashes) is the parent. `..` is parent. A single
 * trailing slash is only the separator. */
static int next_comp(const char **pp, char *out, size_t n, int *parent,
                     int *end)
{
    const char *p = *pp;
    size_t i = 0;

    *parent = 0;
    *end = 0;
    if (*p == 0) {
        *end = 1;
        return 1;
    }
    if (*p == '/') {
        *parent = 1;
        *pp = p + 1;
        return 1;
    }
    while (*p && *p != '/') {
        if (i + 1 >= n)
            return 0;
        out[i++] = *p++;
    }
    out[i] = 0;
    if (*p == '/')
        p++;
    *pp = p;
    return 1;
}

/* Walks path from the ROOT, never above it. Returns the lock of the leaf's
 * parent directory and the leaf name, or 0 with *err. */
static uintptr_t resolve_rel(const struct zzrun_files *f, const char *path,
                             char *leaf, size_t leaf_n, int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    const char *p = path;
    uintptr_t cur = d->dup_lock(d->ctx, f->root);
    char comp[108];
    char hold[108];
    int have_hold = 0;

    hold[0] = 0;
    leaf[0] = 0;
    if (!cur) {
        *err = AAR_E_IO;
        return 0;
    }
    for (;;) {
        int parent = 0, end = 0;

        comp[0] = 0;
        if (!next_comp(&p, comp, sizeof(comp), &parent, &end)) {
            *err = AAR_E_INVAL;
            d->unlock(d->ctx, cur);
            return 0;
        }
        if (end)
            break;
        if (parent || strcmp(comp, "..") == 0) {
            if (have_hold) {
                if (!enter_dir(f, &cur, hold, err)) {
                    d->unlock(d->ctx, cur);
                    return 0;
                }
                have_hold = 0;
            }
            if (!step_parent(f, &cur, err)) {
                d->unlock(d->ctx, cur);
                return 0;
            }
            continue;
        }
        if (strcmp(comp, ".") == 0)
            continue;
        if (have_hold) {
            if (!enter_dir(f, &cur, hold, err)) {
                d->unlock(d->ctx, cur);
                return 0;
            }
        }
        memcpy(hold, comp, strlen(comp) + 1);
        have_hold = 1;
    }
    if (have_hold) {
        if (strlen(hold) >= leaf_n) {
            *err = AAR_E_INVAL;
            d->unlock(d->ctx, cur);
            return 0;
        }
        memcpy(leaf, hold, strlen(hold) + 1);
    }
    return cur;
}

/* An error for a failed lock: the DOS error, or ENOENT when there is none. */
static int32_t lock_errno(int32_t io)
{
    int32_t err = zzrun_errno_from_io(io);

    return err == AAR_OK ? AAR_E_NOENT : err;
}

static int lock_target(const struct zzrun_files *f, uintptr_t dir,
                       const char *leaf, uintptr_t *out, int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t locked;

    if (!leaf[0]) {
        *out = d->dup_lock(d->ctx, dir);
        if (!*out) {
            *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    locked = d->lock_rel(d->ctx, dir, leaf, &transport_ok, &io);
    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!locked) {
        *err = lock_errno(io);
        return 0;
    }
    if (!f->abs_paths && !under_root(f, locked)) {
        d->unlock(d->ctx, locked);
        *err = AAR_E_ACCES;
        return 0;
    }
    *out = locked;
    return 1;
}

static int final_path_under_root(const struct zzrun_files *f, uintptr_t dir,
                                 const char *leaf, enum zzrun_open_plan plan,
                                 int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    int transport_ok = 0;
    int32_t io = 0;
    uintptr_t locked = d->lock_rel(d->ctx, dir, leaf, &transport_ok, &io);

    if (!transport_ok) {
        *err = AAR_E_IO;
        return 0;
    }
    if (!locked) {
        switch (zzrun_leaf_lock_failure(plan, io, ZZRUN_IOERR_OBJECT_NOT_FOUND,
                                        ZZRUN_IOERR_OBJECT_IN_USE)) {
        case ZZRUN_LEAF_PROCEED:
            return 1;
        case ZZRUN_LEAF_EXISTS:
            *err = AAR_E_EXIST;
            return 0;
        default:
            break;
        }
        *err = lock_errno(io);
        return 0;
    }
    if (!under_root(f, locked)) {
        d->unlock(d->ctx, locked);
        *err = AAR_E_ACCES;
        return 0;
    }
    d->unlock(d->ctx, locked);
    return 1;
}

/* A step that creates, truncates or replaces a file runs only while its
 * request is live and no break is pending. */
static int open_step_allowed(const struct zzrun_dos *d)
{
    return zzrun_open_step_allowed(d->live(d->ctx), d->user_break(d->ctx), 0);
}

/* Every open that can create or replace a file (anything but MODE_OLDFILE)
 * checks that the request is still live before its packet goes out. */
static uintptr_t open_target(const struct zzrun_dos *d, enum zzrun_mode mode,
                             int *transport_ok, int32_t *io)
{
    if (mode != ZZRUN_MODE_OLD && !open_step_allowed(d)) {
        *transport_ok = 0;
        *io = ZZRUN_IOERR_BREAK;
        return 0;
    }
    return d->target_open(d->ctx, mode, transport_ok, io);
}

/* Opens one file for the flags word's plan. Returns 1 with *fh_out set, or 0
 * with *err set (still AAR_OK means the DOS error *io says why).
 *
 * AmigaDOS has no atomic exclusive create. EXCL is a Lock probe followed by
 * MODE_NEWFILE, so it is exclusive against the other apps ZZRun serves (one
 * serving loop handles every file request) but not against other Amiga
 * programs that create the same name between the probe and the open. */
static int open_with_plan(const struct zzrun_files *f,
                          enum zzrun_open_plan plan, uintptr_t dir,
                          const char *name, int absolute, uintptr_t *fh_out,
                          int32_t *err, int32_t *io)
{
    const struct zzrun_dos *d = f->dos;
    uintptr_t fh = 0;
    int transport_ok = 0;
    int exists = 0;
    int ok = 0;

    *fh_out = 0;
    *io = 0;
    d->target_begin(d->ctx, absolute ? 0 : dir, name);
    if (zzrun_plan_probes_target(plan, absolute)) {
        if (!d->target_probe(d->ctx, &exists, &transport_ok, io)) {
            if (!transport_ok) {
                *err = AAR_E_IO;
            } else {
                *err = lock_errno(*io);
            }
            goto out;
        }
    }
    switch (plan) {
    case ZZRUN_OPEN_READ_EXISTING:
        fh = open_target(d, ZZRUN_MODE_OLD, &transport_ok, io);
        break;
    case ZZRUN_OPEN_UPDATE_EXISTING:
    case ZZRUN_OPEN_TRUNCATE_EXISTING:
        if (!exists) {
            *err = AAR_E_NOENT;
            goto out;
        }
        fh = open_target(d, ZZRUN_MODE_READWRITE, &transport_ok, io);
        if (fh && plan == ZZRUN_OPEN_TRUNCATE_EXISTING) {
            int32_t size;
            int size_ok = 0;
            int32_t size_io = 0;

            if (!open_step_allowed(d)) {
                d->fh_end_or_close(d->ctx, fh);
                *err = AAR_E_IO;
                goto out;
            }
            size = d->fh_set_size0(d->ctx, fh, &size_ok, &size_io);
            if (!size_ok) {
                d->fh_end_or_close(d->ctx, fh);
                *err = AAR_E_IO;
                goto out;
            }
            if (size < 0) {
                d->fh_end_or_close(d->ctx, fh);
                fh = 0;
                if (size_io != ZZRUN_IOERR_ACTION_NOT_KNOWN) {
                    *err = size_io == ZZRUN_IOERR_DISK_FULL ? AAR_E_NOSPC :
                           size_io == ZZRUN_IOERR_WRITE_PROTECTED ?
                           AAR_E_ACCES : AAR_E_IO;
                    goto out;
                }
                /* No ACTION_SET_FILE_SIZE: the probe found the file, so
                 * MODE_NEWFILE replaces it, on the probe's candidate. */
                fh = open_target(d, ZZRUN_MODE_NEW, &transport_ok, io);
            }
        }
        break;
    case ZZRUN_OPEN_CREATE_KEEP:
        fh = open_target(d, ZZRUN_MODE_READWRITE, &transport_ok, io);
        if (!fh && transport_ok && *io == ZZRUN_IOERR_OBJECT_NOT_FOUND)
            fh = open_target(d, ZZRUN_MODE_NEW, &transport_ok, io);
        break;
    case ZZRUN_OPEN_CREATE_REPLACE:
        fh = open_target(d, ZZRUN_MODE_NEW, &transport_ok, io);
        break;
    case ZZRUN_OPEN_CREATE_EXCLUSIVE:
        if (exists) {
            *err = AAR_E_EXIST;
            goto out;
        }
        fh = open_target(d, ZZRUN_MODE_NEW, &transport_ok, io);
        break;
    default:
        *err = AAR_E_INVAL;
        goto out;
    }
    if (!transport_ok) {
        *err = AAR_E_IO;
        goto out;
    }
    *fh_out = fh;
    ok = fh != 0;
out:
    d->target_end(d->ctx);
    return ok;
}

static int32_t serve_open(struct zzrun_files *f, const struct zzrun_req *req)
{
    const struct zzrun_dos *d = f->dos;
    char path[ZZRUN_PATH_BYTES];
    char leaf[108];
    uintptr_t dir = 0;
    uintptr_t fh = 0;
    enum zzrun_open_plan plan;
    int32_t err = AAR_OK;
    int32_t io = 0;
    int index;
    int absolute = 0;

    index = slot_index((int32_t)req->arg[AAR_A_FILE_OPEN_FD], &err);
    if (index < 0)
        return err;
    if (f->fh[index])
        return AAR_E_BUSY;
    plan = zzrun_open_plan_for_flags(req->arg[AAR_A_FILE_OPEN_FLAGS]);
    if (plan == ZZRUN_OPEN_INVALID)
        return AAR_E_INVAL;
    if (!path_from_req(req, path, &err))
        return err;
    if (path[0] == 0)
        return AAR_E_INVAL;
    if (strchr(path, ':')) {
        if (!f->abs_paths)
            return AAR_E_ACCES;
        absolute = 1;
    }
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!absolute) {
        dir = resolve_rel(f, path, leaf, sizeof(leaf), &err);
        if (!dir)
            return err;
        if (!leaf[0]) {
            err = AAR_E_IO;
            goto done;
        }
        if (!final_path_under_root(f, dir, leaf, plan, &err))
            goto done;
    }
    if (!open_with_plan(f, plan, dir, absolute ? path : leaf, absolute, &fh,
                        &err, &io) &&
        err == AAR_OK)
        err = lock_errno(io);
    if (!fh)
        goto done;
    if (!d->live(d->ctx)) {
        err = AAR_E_IO;
        goto done;
    }
    f->fh[index] = fh;
    fh = 0;
done:
    if (fh)
        d->fh_end_or_close(d->ctx, fh);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_close(struct zzrun_files *f, const struct zzrun_req *req)
{
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    int delivered = 0;
    int32_t io = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (!f->dos->fh_close(f->dos->ctx, f->fh[index], &delivered, &io))
        err = delivered ? lock_errno(io) : AAR_E_IO;
    if (delivered)
        f->fh[index] = 0;
    return err;
}

static int32_t serve_read(struct zzrun_files *f, const struct zzrun_req *req,
                          struct zzrun_done *done)
{
    const struct zzrun_dos *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t left = req->arg[AAR_A_FILE_LEN];
    uint32_t got = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (left > req->buf_len || (left && !req->buf))
        return AAR_E_INVAL;
    while (left) {
        uint32_t chunk = left > ZZRUN_IO_CHUNK ? ZZRUN_IO_CHUNK : left;
        int transport_ok = 0;
        int32_t io = 0;
        int32_t n;

        if (!d->live(d->ctx)) {
            err = AAR_E_IO;
            break;
        }
        if (d->user_break(d->ctx))
            break;
        n = d->fh_read(d->ctx, f->fh[index], f->io, chunk, &transport_ok, &io);
        if (!transport_ok) {
            err = AAR_E_IO;
            break;
        }
        if (n < 0) {
            err = lock_errno(io);
            break;
        }
        if (n == 0)
            break;
        memcpy(req->buf + got, f->io, (uint32_t)n);
        got += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    done->out[AAR_R_FILE_COUNT] = got;
    return err;
}

static int32_t serve_write(struct zzrun_files *f, const struct zzrun_req *req,
                           struct zzrun_done *done)
{
    const struct zzrun_dos *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t left = req->arg[AAR_A_FILE_LEN];
    uint32_t put = 0;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (left > req->buf_len || (left && !req->buf))
        return AAR_E_INVAL;
    while (left) {
        uint32_t chunk = left > ZZRUN_IO_CHUNK ? ZZRUN_IO_CHUNK : left;
        int transport_ok = 0;
        int32_t io = 0;
        int32_t n;

        if (!d->live(d->ctx)) {
            err = AAR_E_IO;
            break;
        }
        memcpy(f->io, req->buf + put, chunk);
        n = d->fh_write(d->ctx, f->fh[index], f->io, chunk, &transport_ok,
                        &io);
        if (!transport_ok) {
            err = AAR_E_IO;
            break;
        }
        if (n < 0) {
            err = lock_errno(io);
            break;
        }
        if (n == 0) {
            err = AAR_E_NOSPC;
            break;
        }
        put += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    done->out[AAR_R_FILE_COUNT] = put;
    return err;
}

static int32_t serve_seek(struct zzrun_files *f, const struct zzrun_req *req,
                          struct zzrun_done *done)
{
    const struct zzrun_dos *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_FILE_FD], &err);
    uint32_t whence = req->arg[AAR_A_FILE_WHENCE];
    uint64_t offset = ((uint64_t)req->arg[AAR_A_FILE_OFF_HI] << 32) |
                      req->arg[AAR_A_FILE_OFF_LO];
    int64_t target = (int64_t)offset;
    int mode;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t pos;
    int32_t current;

    if (index < 0 || !f->fh[index])
        return AAR_E_BADF;
    if (whence == (uint32_t)AAR_SEEK_SET) {
        if (offset > 0x7fffffffULL)
            return AAR_E_INVAL;
        mode = 0;
    } else if (whence == (uint32_t)AAR_SEEK_CUR) {
        mode = 1;
    } else if (whence == (uint32_t)AAR_SEEK_END) {
        mode = 2;
    } else {
        return AAR_E_INVAL;
    }
    if (target > 2147483647LL || target < -2147483647LL)
        return AAR_E_INVAL;
    pos = d->fh_seek(d->ctx, f->fh[index], (int32_t)target, mode,
                     &transport_ok, &io);
    if (!transport_ok)
        return AAR_E_IO;
    if (pos < 0)
        return lock_errno(io);
    current = d->fh_seek(d->ctx, f->fh[index], 0, 1, &transport_ok, &io);
    if (!transport_ok)
        return AAR_E_IO;
    if (current < 0)
        return lock_errno(io);
    done->out[AAR_R_FILE_POS_HI] = 0;
    done->out[AAR_R_FILE_POS_LO] = (uint32_t)current;
    return AAR_OK;
}

/* The lock a path names for stat and list: an absolute name through the
 * resolver, a relative one confined to the ROOT. *dir is the parent lock
 * the caller unlocks. */
static int lock_named(struct zzrun_files *f, const char *path,
                      int allow_empty_root, uintptr_t *dir, uintptr_t *locked,
                      int32_t *err)
{
    const struct zzrun_dos *d = f->dos;
    char leaf[108];
    int transport_ok = 0;
    int32_t io = 0;

    *dir = 0;
    *locked = 0;
    if (strchr(path, ':')) {
        if (!f->abs_paths) {
            *err = AAR_E_ACCES;
            return 0;
        }
        *locked = d->lock_abs(d->ctx, path, &transport_ok, &io);
        if (!transport_ok) {
            *err = AAR_E_IO;
            return 0;
        }
        if (!*locked) {
            *err = zzrun_errno_from_io(io);
            if (*err == AAR_OK)
                *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    if (path[0] == 0 && allow_empty_root) {
        *locked = d->dup_lock(d->ctx, f->root);
        if (!*locked) {
            *err = AAR_E_IO;
            return 0;
        }
        return 1;
    }
    *dir = resolve_rel(f, path, leaf, sizeof(leaf), err);
    if (!*dir)
        return 0;
    if (!lock_target(f, *dir, leaf, locked, err))
        return 0;
    return 1;
}

static int32_t serve_stat(struct zzrun_files *f, const struct zzrun_req *req,
                          struct zzrun_done *done)
{
    const struct zzrun_dos *d = f->dos;
    char path[ZZRUN_PATH_BYTES];
    uintptr_t dir = 0;
    uintptr_t locked = 0;
    struct zzrun_info info;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t err = AAR_OK;

    if (!path_from_req(req, path, &err))
        return err;
    if (path[0] == 0)
        return AAR_E_INVAL;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!lock_named(f, path, 0, &dir, &locked, &err))
        goto done;
    if (!d->lock_examine(d->ctx, locked, &info, &transport_ok, &io)) {
        err = transport_ok ? lock_errno(io) : AAR_E_IO;
        goto done;
    }
    done->out[AAR_R_STAT_SIZE_HI] = 0;
    done->out[AAR_R_STAT_SIZE_LO] = info.size;
    done->out[AAR_R_STAT_MTIME_HI] = 0;
    done->out[AAR_R_STAT_MTIME_LO] = info.mtime;
    done->out[AAR_R_STAT_MODE] = info.is_dir ? AAR_MODE_DIR : AAR_MODE_FILE;
done:
    if (locked)
        d->unlock(d->ctx, locked);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_list_open(struct zzrun_files *f,
                               const struct zzrun_req *req)
{
    const struct zzrun_dos *d = f->dos;
    char path[ZZRUN_PATH_BYTES];
    uintptr_t dir = 0;
    uintptr_t locked = 0;
    uintptr_t list = 0;
    int transport_ok = 0;
    int32_t io = 0;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);

    if (index < 0)
        return err;
    if (f->list[index])
        return AAR_E_BUSY;
    if (!path_from_req(req, path, &err))
        return err;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!lock_named(f, path, 1, &dir, &locked, &err))
        goto done;
    if (!d->list_open(d->ctx, locked, &list, &transport_ok, &io)) {
        err = transport_ok ? lock_errno(io) : AAR_E_IO;
        goto done;
    }
    /* The list owns the lock now. */
    locked = 0;
    if (!d->live(d->ctx)) {
        d->list_close(d->ctx, list);
        err = AAR_E_IO;
        goto done;
    }
    f->list[index] = list;
done:
    if (locked)
        d->unlock(d->ctx, locked);
    if (dir)
        d->unlock(d->ctx, dir);
    return err;
}

static int32_t serve_list_next(struct zzrun_files *f,
                               const struct zzrun_req *req,
                               struct zzrun_done *done)
{
    const struct zzrun_dos *d = f->dos;
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);
    struct zzrun_info info;
    int transport_ok = 0;
    int32_t io = 0;
    size_t len;

    if (index < 0 || !f->list[index])
        return AAR_E_BADF;
    if (!req->buf || req->buf_len < 2u)
        return AAR_E_INVAL;
    if (!d->live(d->ctx))
        return AAR_E_IO;
    if (!d->list_next(d->ctx, f->list[index], &info, &transport_ok, &io))
        return transport_ok ? lock_errno(io) : AAR_E_IO;
    len = strlen(info.name);
    if (len > AAR_NAME_MAX)
        len = AAR_NAME_MAX;
    if (len > req->buf_len - 1u)
        len = req->buf_len - 1u;
    memcpy(req->buf, info.name, len);
    req->buf[len] = 0;
    done->out[AAR_R_LIST_MODE] = info.is_dir ? AAR_MODE_DIR : AAR_MODE_FILE;
    return AAR_OK;
}

static int32_t serve_list_close(struct zzrun_files *f,
                                const struct zzrun_req *req)
{
    int32_t err = AAR_OK;
    int index = slot_index((int32_t)req->arg[AAR_A_LIST_ID], &err);

    if (index < 0 || !f->list[index])
        return AAR_E_BADF;
    f->dos->list_close(f->dos->ctx, f->list[index]);
    f->list[index] = 0;
    return AAR_OK;
}

void zzrun_files_serve(struct zzrun_files *f, const struct zzrun_req *req,
                       struct zzrun_done *done)
{
    int32_t result;

    done_clear(done);
    switch (req->op) {
    case AAR_OP_FILE_OPEN: result = serve_open(f, req); break;
    case AAR_OP_FILE_CLOSE: result = serve_close(f, req); break;
    case AAR_OP_FILE_READ: result = serve_read(f, req, done); break;
    case AAR_OP_FILE_WRITE: result = serve_write(f, req, done); break;
    case AAR_OP_FILE_SEEK: result = serve_seek(f, req, done); break;
    case AAR_OP_FILE_STAT: result = serve_stat(f, req, done); break;
    case AAR_OP_LIST_OPEN: result = serve_list_open(f, req); break;
    case AAR_OP_LIST_NEXT: result = serve_list_next(f, req, done); break;
    case AAR_OP_LIST_CLOSE: result = serve_list_close(f, req); break;
    default: result = AAR_E_NOSYS; break;
    }
    done->result = result;
}

/* ---- Console ------------------------------------------------------------ */

void zzrun_console_init(struct zzrun_console *c,
                        const struct zzrun_console_ops *ops,
                        const struct zzrun_sink *sink)
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    c->sink = sink;
}

static void launcher_put_byte(struct zzrun_console *c, uint8_t value)
{
    c->launcher[c->launcher_head] = value;
    c->launcher_head = (c->launcher_head + 1u) % ZZRUN_LAUNCHER_OUTPUT_BYTES;
    c->launcher_used++;
}

void zzrun_console_launcher_put(struct zzrun_console *c, const uint8_t *data,
                                uint32_t bytes, int diagnostic)
{
    uint32_t i;

    if (!zzrun_launcher_output_accept(c->launcher_used, bytes, diagnostic)) {
        if (diagnostic)
            c->launcher_dropped++;
        return;
    }
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, data[i]);
}

void zzrun_console_launcher_status(struct zzrun_console *c, const char *msg)
{
    uint32_t bytes = (uint32_t)strlen(msg);
    uint32_t i;

    if (!zzrun_launcher_output_accept(c->launcher_used, bytes + 1u, 0))
        return;
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, (uint8_t)msg[i]);
    launcher_put_byte(c, '\n');
}

void zzrun_console_launcher_log(struct zzrun_console *c, const uint8_t *data,
                                uint32_t bytes)
{
    uint32_t i;
    int newline = bytes == 0 || data[bytes - 1u] != '\n';

    if (!zzrun_launcher_output_accept(c->launcher_used,
                                      bytes + (uint32_t)newline, 1)) {
        c->launcher_dropped++;
        return;
    }
    for (i = 0; i < bytes; i++)
        launcher_put_byte(c, data[i]);
    if (newline)
        launcher_put_byte(c, '\n');
}

static void app_write_finish(struct zzrun_console *c, int32_t result)
{
    struct zzrun_done done;

    done_result(&done, result);
    done.out[AAR_R_CONSOLE_COUNT] = c->app_write_done;
    c->app_write_active = 0;
    c->sink->complete(c->sink->ctx, &c->app_write, &done);
}

static void read_finish(struct zzrun_console *c, int32_t result,
                        uint32_t count)
{
    struct zzrun_done done;

    done_result(&done, result);
    done.out[AAR_R_CONSOLE_COUNT] = count;
    c->read_active = 0;
    c->read_pending = 0;
    c->sink->complete(c->sink->ctx, &c->read_req, &done);
}

static void console_fail(struct zzrun_console *c)
{
    if (c->failed)
        return;
    c->failed = 1;
    c->ops.output_error(c->ops.ctx);
}

static uint32_t launcher_contiguous(const struct zzrun_console *c)
{
    uint32_t bytes = ZZRUN_LAUNCHER_OUTPUT_BYTES - c->launcher_tail;

    return c->launcher_used < bytes ? c->launcher_used : bytes;
}

void zzrun_console_pump(struct zzrun_console *c)
{
    if (c->failed && c->app_write_active)
        app_write_finish(c, AAR_E_IO);
    if (!c->failed && !c->write_pending) {
        uint32_t app_left = c->app_write_active ?
            c->app_write.buf_len - c->app_write_done : 0u;
        int source = zzrun_output_next_source(c->launcher_used, app_left);

        if (source == ZZRUN_OUTPUT_APP && !c->ops.open(c->ops.ctx)) {
            console_fail(c);
            app_write_finish(c, AAR_E_IO);
        } else if (source == ZZRUN_OUTPUT_LAUNCHER) {
            uint32_t chunk = launcher_contiguous(c);

            if (chunk > ZZRUN_CONSOLE_WRITE_BYTES)
                chunk = ZZRUN_CONSOLE_WRITE_BYTES;
            c->write_source = source;
            c->write_bytes = chunk;
            c->write_pending = 1;
            if (!c->ops.write_start(c->ops.ctx, c->launcher + c->launcher_tail,
                                    chunk)) {
                c->write_pending = 0;
                console_fail(c);
            }
        } else if (source == ZZRUN_OUTPUT_APP) {
            uint32_t chunk = app_left;

            if (chunk > ZZRUN_CONSOLE_WRITE_BYTES)
                chunk = ZZRUN_CONSOLE_WRITE_BYTES;
            c->write_source = source;
            c->write_bytes = chunk;
            c->write_pending = 1;
            if (!c->ops.write_start(c->ops.ctx,
                                    c->app_write.buf + c->app_write_done,
                                    chunk)) {
                c->write_pending = 0;
                console_fail(c);
                app_write_finish(c, AAR_E_IO);
            }
        }
    }
    if (c->read_active && !c->read_pending && !c->eof &&
        c->ops.read_allowed(c->ops.ctx)) {
        if (!c->ops.open(c->ops.ctx)) {
            read_finish(c, AAR_E_IO, 0);
        } else if (c->ops.read_start(c->ops.ctx, c->read_max)) {
            c->read_pending = 1;
        } else {
            /* A read that cannot start is the end of the input. */
            c->eof = 1;
            read_finish(c, AAR_OK, 0);
        }
    }
}

void zzrun_console_write_request(struct zzrun_console *c,
                                 const struct zzrun_req *req,
                                 struct zzrun_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (c->failed) {
        done->result = AAR_E_IO;
        return;
    }
    if (req->buf_len == 0) {
        done->result = AAR_OK;
        return;
    }
    if (!req->buf) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (c->app_write_active) {
        done->result = AAR_E_BUSY;
        return;
    }
    c->app_write = *req;
    c->app_write_done = 0;
    c->app_write_active = 1;
    *deferred = 1;
    zzrun_console_pump(c);
}

void zzrun_console_write_done(struct zzrun_console *c, int32_t written)
{
    uint32_t advanced = zzrun_console_write_advance(c->write_bytes, written);
    int source = c->write_source;
    int failed = zzrun_console_write_failed(c->write_bytes, written);

    c->write_pending = 0;
    c->write_source = ZZRUN_OUTPUT_NONE;
    if (source == ZZRUN_OUTPUT_LAUNCHER) {
        c->launcher_tail = (c->launcher_tail + advanced) %
                           ZZRUN_LAUNCHER_OUTPUT_BYTES;
        c->launcher_used -= advanced;
    } else if (c->app_write_active) {
        c->app_write_done += advanced;
    }
    if (failed) {
        console_fail(c);
        if (c->app_write_active)
            app_write_finish(c, AAR_E_IO);
        return;
    }
    if (source == ZZRUN_OUTPUT_APP && c->app_write_active &&
        c->app_write_done >= c->app_write.buf_len)
        app_write_finish(c, AAR_OK);
    zzrun_console_pump(c);
}

void zzrun_console_read_request(struct zzrun_console *c,
                                const struct zzrun_req *req,
                                struct zzrun_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (!req->buf || req->buf_len == 0) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (c->eof) {
        done->result = AAR_OK;
        done->out[AAR_R_CONSOLE_COUNT] = 0;
        return;
    }
    if (c->read_active) {
        done->result = AAR_E_BUSY;
        return;
    }
    c->read_req = *req;
    c->read_active = 1;
    c->read_pending = 0;
    c->read_max = req->buf_len < ZZRUN_CONSOLE_READ_BYTES ? req->buf_len :
                                                          ZZRUN_CONSOLE_READ_BYTES;
    *deferred = 1;
    zzrun_console_pump(c);
}

void zzrun_console_read_done(struct zzrun_console *c, int32_t got,
                             const uint8_t *data)
{
    uint32_t count;

    if (!c->read_active) {
        c->read_pending = 0;
        return;
    }
    if (got <= 0) {
        c->eof = 1;
        read_finish(c, AAR_OK, 0);
        return;
    }
    count = (uint32_t)got < c->read_req.buf_len ? (uint32_t)got :
                                                  c->read_req.buf_len;
    memcpy(c->read_req.buf, data, count);
    read_finish(c, AAR_OK, count);
}

int zzrun_console_output_pending(const struct zzrun_console *c)
{
    return c->write_pending || c->launcher_used || c->app_write_active;
}

/* ---- Display ------------------------------------------------------------ */

void zzrun_display_init(struct zzrun_display *d,
                        const struct zzrun_display_ops *ops,
                        const struct zzrun_sink *sink)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    d->sink = sink;
}

void zzrun_display_reset(struct zzrun_display *d)
{
    d->open = 0;
    d->have_two = 0;
    d->have_pan = 0;
    d->pan_count = 0;
    d->showing = 0;
    d->flip_active = 0;
    d->flip_queued = 0;
}

int32_t zzrun_display_open_check(const struct zzrun_req *req)
{
    uint32_t w = req->arg[AAR_A_DISPLAY_OPEN_W];
    uint32_t h = req->arg[AAR_A_DISPLAY_OPEN_H];
    uint32_t format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];

    if (w == 0 || h == 0 || w > ZZRUN_SCREEN_MAX || h > ZZRUN_SCREEN_MAX)
        return AAR_E_INVAL;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&
        format != AAR_FMT_CLUT8)
        return AAR_E_INVAL;
    return AAR_OK;
}

uint32_t zzrun_display_depth(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? 32u :
           (format == AAR_FMT_CLUT8 ? 8u : 16u);
}

uint32_t zzrun_display_layout(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? AAR_LAYOUT_BGRA8888 :
           (format == AAR_FMT_CLUT8 ? AAR_LAYOUT_CLUT8 : AAR_LAYOUT_RGB565_BE);
}

uint16_t zzrun_display_caps(uint32_t open_flags)
{
    return (uint16_t)(AAR_DISPCAP_ASYNC | AAR_DISPCAP_CLUT8 |
                      ((open_flags & AAR_DISPLAY_LATEST) ?
                       AAR_DISPCAP_LATEST : 0u));
}

void zzrun_display_opened(struct zzrun_display *d, const struct zzrun_req *req,
                          uint32_t pitch, uint32_t front, uint32_t back,
                          uint32_t spare, int have_two, int have_pan,
                          uint32_t pan_count, struct zzrun_done *done)
{
    zzrun_display_reset(d);
    d->open = 1;
    d->id = ZZRUN_DISPLAY_ID;
    d->width = req->arg[AAR_A_DISPLAY_OPEN_W];
    d->height = req->arg[AAR_A_DISPLAY_OPEN_H];
    d->format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];
    d->flags = req->arg[AAR_A_DISPLAY_OPEN_FLAGS];
    d->layout = zzrun_display_layout(d->format);
    d->pitch = pitch;
    d->front = front;
    d->back = back;
    d->spare = spare;
    d->have_two = have_two;
    d->have_pan = have_pan;
    d->pan_count = have_pan ? pan_count : 0u;
    done_result(done, AAR_OK);
    done->flags = zzrun_display_caps(d->flags);
    done->out[AAR_R_DISPLAY_OPEN_ID] = d->id;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = pitch;
    done->out[AAR_R_DISPLAY_OPEN_LAYOUT] = d->layout;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = front;
    done->out[AAR_R_DISPLAY_OPEN_BACK] = back;
    done->out[AAR_R_DISPLAY_OPEN_SPARE] = spare;
}

static uint32_t display_pages(const struct zzrun_display *d)
{
    if (d->have_pan)
        return d->pan_count ? d->pan_count : 1u;
    return d->have_two ? 2u : 1u;
}

static uint32_t display_page_token(const struct zzrun_display *d, uint32_t page)
{
    if (d->have_pan)
        return d->front + page * d->pitch * d->height;
    return page == 0 ? d->front : (page == 1 ? d->back : d->spare);
}

/* After a flip: the page now scanned out is the front, the next page the app
 * draws into is the back. */
static void display_flip_outs(const struct zzrun_display *d,
                              struct zzrun_done *done)
{
    uint32_t pages = display_pages(d);

    done->out[AAR_R_DISPLAY_FLIP_FRONT] = display_page_token(d, d->showing);
    done->out[AAR_R_DISPLAY_FLIP_BACK] =
        display_page_token(d, (d->showing + 1u) % pages);
    done->out[AAR_R_DISPLAY_FLIP_PITCH] = d->pitch;
    done->out[AAR_R_DISPLAY_FLIP_LAYOUT] = d->layout;
    done->out[AAR_R_DISPLAY_FLIP_DROPPED] = 0;
}

static int display_flip_two(struct zzrun_display *d, const struct zzrun_req *req)
{
    uint32_t next = d->showing ? 0u : 1u;

    if (!d->ops.change_buffer(d->ops.ctx, (int)next))
        return 0;
    d->showing = next;
    d->flip_req = *req;
    d->flip_active = 1;
    return 1;
}

void zzrun_display_flip_request(struct zzrun_display *d,
                                const struct zzrun_req *req,
                                struct zzrun_done *done, int *deferred)
{
    *deferred = 0;
    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (d->have_two) {
        if (d->flip_active) {
            if (d->flip_queued) {
                done->result = AAR_E_BUSY;
                return;
            }
            d->flip_next = *req;
            d->flip_queued = 1;
            *deferred = 1;
            return;
        }
        if (!display_flip_two(d, req)) {
            done->result = AAR_E_IO;
            return;
        }
        *deferred = 1;
        return;
    }
    if (d->have_pan) {
        uint32_t next = (d->showing + 1u) % display_pages(d);

        d->ops.pan(d->ops.ctx, next);
        d->showing = next;
        done->flags = ZZAPP_FLIPF_SUBMITTED;
        display_flip_outs(d, done);
        return;
    }
    /* A single buffer: the firmware copies the frame. */
    if (d->flags & AAR_DISPLAY_VSYNC)
        d->ops.wait_tof(d->ops.ctx);
    display_flip_outs(d, done);
}

static void flip_complete(struct zzrun_display *d, const struct zzrun_req *req,
                          int32_t result)
{
    struct zzrun_done done;

    done_result(&done, result);
    if (result == AAR_OK)
        display_flip_outs(d, &done);
    d->sink->complete(d->sink->ctx, req, &done);
}

void zzrun_display_flip_safe(struct zzrun_display *d)
{
    struct zzrun_req finished;

    if (!d->flip_active)
        return;
    finished = d->flip_req;
    d->flip_active = 0;
    flip_complete(d, &finished, AAR_OK);
    if (d->flip_queued) {
        struct zzrun_req next = d->flip_next;

        d->flip_queued = 0;
        if (display_flip_two(d, &next))
            return;
        flip_complete(d, &next, AAR_E_IO);
    }
}

void zzrun_display_palette_request(struct zzrun_display *d,
                                   const struct zzrun_req *req,
                                   struct zzrun_done *done)
{
    uint32_t colors[2u + 256u * 3u];
    uint32_t first = req->arg[AAR_A_DISPLAY_PALETTE_FIRST];
    uint32_t count = req->arg[AAR_A_DISPLAY_PALETTE_COUNT];
    uint32_t i;

    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id ||
        d->format != AAR_FMT_CLUT8 || count == 0 || first >= 256u ||
        count > 256u - first || !req->buf || req->buf_len < count * 4u) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* LoadRGB32: (count << 16) | first, then R, G, B per colour, each
     * left-justified in 32 bits, then a 0 terminator. */
    colors[0] = (count << 16) | first;
    for (i = 0; i < count; i++) {
        uint32_t rgb = zzrun_be32(req->buf + i * 4u);

        colors[1u + i * 3u] = ((rgb >> 16) & 0xffu) * 0x01010101u;
        colors[2u + i * 3u] = ((rgb >> 8) & 0xffu) * 0x01010101u;
        colors[3u + i * 3u] = (rgb & 0xffu) * 0x01010101u;
    }
    colors[1u + count * 3u] = 0;
    d->ops.load_rgb32(d->ops.ctx, colors);
    done->result = AAR_OK;
}

void zzrun_display_close_request(struct zzrun_display *d,
                                 const struct zzrun_req *req,
                                 struct zzrun_done *done)
{
    done_clear(done);
    if (!d->open || req->arg[AAR_A_DISPLAY_ID] != d->id) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* The close waits for the SafeMessages of flips still in flight. */
    d->ops.close(d->ops.ctx);
    if (d->flip_active) {
        struct zzrun_req finished = d->flip_req;

        d->flip_active = 0;
        flip_complete(d, &finished, AAR_E_IO);
    }
    if (d->flip_queued) {
        struct zzrun_req next = d->flip_next;

        d->flip_queued = 0;
        flip_complete(d, &next, AAR_E_IO);
    }
    zzrun_display_reset(d);
    done->result = AAR_OK;
}

/* ---- Audio -------------------------------------------------------------- */

void zzrun_audio_init(struct zzrun_audio *a, const struct zzrun_audio_ops *ops)
{
    memset(a, 0, sizeof(*a));
    a->ops = *ops;
}

void zzrun_audio_open_request(struct zzrun_audio *a, const struct zzrun_req *req,
                              struct zzrun_done *done)
{
    uint32_t rate = req->arg[AAR_A_AUDIO_OPEN_RATE];
    uint32_t channels = req->arg[AAR_A_AUDIO_OPEN_CHANNELS];
    uint32_t route = req->arg[AAR_A_AUDIO_OPEN_ROUTE];
    uint32_t frames = 0;
    uint32_t room;

    done_clear(done);
    /* The platform route never reaches the client. */
    if (route != AAR_AUDIO_ROUTE_AHI || rate == 0 || channels != 2u ||
        req->buf_token == 0 || req->buf_off != 0 ||
        req->buf_len <= AAR_AUDIO_RING_HDR_BYTES) {
        done->result = AAR_E_INVAL;
        return;
    }
    /* The ring holds a power of two of frames after the header. */
    room = (req->buf_len - AAR_AUDIO_RING_HDR_BYTES) / (channels * 2u);
    for (frames = 1u; frames <= room / 2u; frames <<= 1)
        ;
    if (room == 0) {
        done->result = AAR_E_INVAL;
        return;
    }
    if (a->open) {
        a->ops.close(a->ops.ctx);
        a->open = 0;
    }
    if (!a->ops.open(a->ops.ctx, rate, channels, req->buf_token, req->buf_len,
                     frames)) {
        a->open_failed = 1;
        done->result = AAR_E_IO;
        return;
    }
    a->open = 1;
    a->rate = rate;
    a->channels = channels;
    a->frames = frames;
    done->result = AAR_OK;
    done->out[AAR_R_AUDIO_OPEN_RATE] = rate;
    done->out[AAR_R_AUDIO_OPEN_CHANNELS] = channels;
    done->out[AAR_R_AUDIO_OPEN_RING] = req->buf_token;
    done->out[AAR_R_AUDIO_OPEN_RING_BYTES] = req->buf_len;
    done->out[AAR_R_AUDIO_OPEN_FRAMES] = frames;
}

void zzrun_audio_close_request(struct zzrun_audio *a,
                               const struct zzrun_req *req,
                               struct zzrun_done *done)
{
    (void)req;
    done_clear(done);
    if (a->open) {
        a->ops.close(a->ops.ctx);
        a->open = 0;
    }
    done->result = AAR_OK;
}

uint32_t zzrun_audio_chunk_frames(uint32_t available_frames, uint32_t rate,
                                  uint32_t channels, uint32_t buffer_bytes)
{
    uint32_t frames = available_frames;
    uint32_t tick = rate / 50u;
    uint32_t fit = channels ? buffer_bytes / (channels * 2u) : 0u;

    if (tick < frames)
        frames = tick;
    if (fit < frames)
        frames = fit;
    return frames;
}

/* ---- Events ------------------------------------------------------------- */

void zzrun_events_init(struct zzrun_events *e, const struct zzrun_events_ops *ops)
{
    memset(e, 0, sizeof(*e));
    e->ops = *ops;
}

int zzrun_events_push(struct zzrun_events *e, uint16_t type, uint16_t code,
                      int16_t dx, int16_t dy, uint16_t buttons,
                      uint32_t time_ms)
{
    aar_event ev;
    int rc;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.time_ms = time_ms;
    if (type == AAR_EV_STOP) {
        if (e->stop_pending)
            return AAR_OK;
        rc = e->ops.post(e->ops.ctx, &ev);
        if (rc == AAR_E_BUSY) {
            e->stop_pending = 1;
            e->stop = ev;
            return AAR_OK;
        }
        return rc;
    }
    return e->ops.post(e->ops.ctx, &ev);
}

void zzrun_events_flush(struct zzrun_events *e)
{
    if (e->stop_pending && e->ops.post(e->ops.ctx, &e->stop) != AAR_E_BUSY)
        e->stop_pending = 0;
}

int zzrun_events_pending(const struct zzrun_events *e)
{
    return e->stop_pending;
}

/* ---- Dispatch ----------------------------------------------------------- */

void zzrun_serve_init(struct zzrun_serve *s, const struct zzrun_sink *sink)
{
    memset(s, 0, sizeof(*s));
    s->sink = *sink;
}

int zzrun_serve_request(struct zzrun_serve *s, const struct zzrun_req *req,
                        struct zzrun_done *done)
{
    int deferred = 0;

    switch (req->op) {
    case AAR_OP_FILE_OPEN:
    case AAR_OP_FILE_CLOSE:
    case AAR_OP_FILE_READ:
    case AAR_OP_FILE_WRITE:
    case AAR_OP_FILE_SEEK:
    case AAR_OP_FILE_STAT:
    case AAR_OP_LIST_OPEN:
    case AAR_OP_LIST_NEXT:
    case AAR_OP_LIST_CLOSE:
        zzrun_files_serve(&s->files, req, done);
        break;
    case AAR_OP_CONSOLE_WRITE:
        zzrun_console_write_request(&s->console, req, done, &deferred);
        break;
    case AAR_OP_CONSOLE_READ:
        zzrun_console_read_request(&s->console, req, done, &deferred);
        break;
    case AAR_OP_DISPLAY_OPEN:
        done_clear(done);
        if (zzrun_display_open_check(req) != AAR_OK)
            done->result = AAR_E_INVAL;
        else if (s->display.open)
            done->result = AAR_E_BUSY;
        else
            s->display.ops.open(s->display.ops.ctx, req, done);
        break;
    case AAR_OP_DISPLAY_FLIP:
        zzrun_display_flip_request(&s->display, req, done, &deferred);
        break;
    case AAR_OP_DISPLAY_PALETTE:
        zzrun_display_palette_request(&s->display, req, done);
        break;
    case AAR_OP_DISPLAY_CLOSE:
        zzrun_display_close_request(&s->display, req, done);
        break;
    case AAR_OP_AUDIO_OPEN:
        zzrun_audio_open_request(&s->audio, req, done);
        break;
    case AAR_OP_AUDIO_CLOSE:
        zzrun_audio_close_request(&s->audio, req, done);
        break;
    default:
        done_result(done, AAR_E_NOSYS);
        break;
    }
    return deferred ? ZZRUN_SERVE_DEFERRED : ZZRUN_SERVE_DONE;
}
