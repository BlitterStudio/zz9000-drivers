/* Shared policy implementation: compiled by host tests and included by
 * ZZRun.c because the Amiga build intentionally has one translation unit. */
#include <stdint.h>
#include <string.h>

#ifndef ZZRUN_POLICY_TYPES_DEFINED
struct zzrun_dos_release_ops {
    void *context;
    int (*send_release)(void *context, int32_t *result);
    void (*free_local)(void *context);
    void (*fallback_release)(void *context);
    void (*forget_slot)(void *context);
};

struct zzrun_shared_writer {
    void *context;
    uint32_t owned_generation;
    int attached;
    uint32_t (*read_current_generation)(void *context);
};
#endif

static void zzrun_policy_put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static int zzrun_dos_release(const struct zzrun_dos_release_ops *ops,
                             int32_t *result)
{
    if (!ops || !ops->send_release)
        return 0;
    return ops->send_release(ops->context, result);
}

int zzrun_dos_close_slot(const struct zzrun_dos_release_ops *ops,
                         int *delivered_out)
{
    int32_t result = 0;
    int delivered = zzrun_dos_release(ops, &result);

    if (delivered_out)
        *delivered_out = delivered;
    if (!delivered)
        return 0;
    if (ops && ops->free_local)
        ops->free_local(ops->context);
    if (ops && ops->forget_slot)
        ops->forget_slot(ops->context);
    return result != 0;
}

void zzrun_dos_end_or_close(const struct zzrun_dos_release_ops *ops)
{
    int32_t result;

    if (!zzrun_dos_release(ops, &result)) {
        if (ops && ops->fallback_release)
            ops->fallback_release(ops->context);
        return;
    }
    if (ops && ops->free_local)
        ops->free_local(ops->context);
}

void zzrun_dos_release_abandoned(const struct zzrun_dos_release_ops *ops,
                                 int acquired)
{
    if (acquired)
        zzrun_dos_end_or_close(ops);
}

static int zzrun_shared_writer_owns(const struct zzrun_shared_writer *writer)
{
    return writer && writer->attached && writer->owned_generation != 0 &&
           writer->read_current_generation &&
           writer->read_current_generation(writer->context) ==
               writer->owned_generation;
}

int zzrun_shared_write_be32(const struct zzrun_shared_writer *writer,
                            uint8_t *out, uint32_t value)
{
    if (!out || !zzrun_shared_writer_owns(writer))
        return 0;
    zzrun_policy_put_be32(out, value);
    return 1;
}

int zzrun_shared_write_bytes(const struct zzrun_shared_writer *writer,
                             uint8_t *out, const uint8_t *in, uint32_t bytes)
{
    if ((!out && bytes) || (!in && bytes) || !zzrun_shared_writer_owns(writer))
        return 0;
    memcpy(out, in, bytes);
    return 1;
}

int zzrun_file_service_generation_owned(uint32_t owned_generation,
                                        uint32_t sampled_generation)
{
    return owned_generation != 0 &&
           owned_generation == sampled_generation;
}
