/*
 * m68k harness for fw_release_m68k.sh: extract BOOT.bin from a release
 * ZIP with the production extractor and inflater, under vamos.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>

#include "fw_release.h"

static FILE *out;

static int sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return fwrite(data, 1, len, out) != len;
}

int main(int argc, char **argv)
{
    static unsigned char buf[1517]; /* odd size: chunks split headers */
    struct fwrel_zip *z;
    FILE *in;
    size_t n;
    int rc = FWREL_ZIP_MORE;

    if (argc != 3)
        return 20;
    in = fopen(argv[1], "rb");
    out = fopen(argv[2], "wb");
    z = fwrel_zip_new(sink, NULL);
    if (!in || !out || !z)
        return 20;
    while (rc == FWREL_ZIP_MORE && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        rc = fwrel_zip_feed(z, buf, n);
    fclose(out);
    fclose(in);
    printf("%s, %lu bytes\n", fwrel_zip_strerror(rc), fwrel_zip_written(z));
    fwrel_zip_free(z);
    return rc == FWREL_ZIP_DONE ? 0 : 10;
}
