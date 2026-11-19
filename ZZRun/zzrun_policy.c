/* Shared policy implementation: compiled by host tests and included by
 * ZZRun.c because the Amiga build intentionally has one translation unit. */
#include <stdint.h>
#include <string.h>

#include "zzrun_policy.h"

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
