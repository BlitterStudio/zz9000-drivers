/*
 * Portable helpers for the online firmware update: release tag and
 * version comparison, board-variant identification, GitHub release JSON,
 * HTTP response parsing and streaming BOOT.bin extraction from a release
 * ZIP. Nothing here touches AmigaOS or the network, so the host suite in
 * common/tests exercises it directly.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FW_RELEASE_H
#define FW_RELEASE_H

#include <stddef.h>
#include <stdint.h>

/* --- Versions ----------------------------------------------------------- */

struct fwrel_version {
    unsigned major;
    unsigned minor;
    unsigned patch;
};

/* Parse a release tag "vMAJOR.MINOR.PATCH" (no suffix, each part 0..255).
 * Returns 1 on success. */
int fwrel_parse_tag(const char *tag, struct fwrel_version *out);

enum fwrel_compare {
    FWREL_UP_TO_DATE,      /* latest equals the installed version */
    FWREL_NEWER,           /* latest is newer */
    FWREL_INSTALLED_NEWER, /* installed is newer (development build) */
    FWREL_PATCH_UNKNOWN    /* same major.minor, latest has a patch level the
                              card cannot report */
};

/* Compare the installed version with the latest release. Without
 * `patch_known` only major.minor are trusted; a matching major.minor is
 * then up to date only when the latest patch is 0. */
enum fwrel_compare fwrel_compare(const struct fwrel_version *installed,
                                 int patch_known,
                                 const struct fwrel_version *latest);

/* --- Board variants ------------------------------------------------------ */

/* Release asset suffixes, in the order the user picks them. */
enum fwrel_variant {
    FWREL_VARIANT_ZORRO3,
    FWREL_VARIANT_ZORRO3_AGA,
    FWREL_VARIANT_ZORRO2,
    FWREL_VARIANT_ZORRO2_2MB,
    FWREL_VARIANT_A500,
    FWREL_VARIANT_A500_2MB,
    FWREL_VARIANT_A500PLUS,
    FWREL_VARIANT_COUNT,
    FWREL_VARIANT_UNKNOWN = -1
};

/* "zorro3", "zorro3-aga", ...; NULL for an out-of-range value. */
const char *fwrel_variant_name(int variant);
/* Short description for the chooser, e.g. "Zorro III, A4000 AGA capture". */
const char *fwrel_variant_label(int variant);
/* 1 for the Zorro III variants. */
int fwrel_variant_is_zorro3(int variant);

/* Variant-word ABI (bits 31..16) of ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG. */
#define FWREL_DIAG_VARIANT_ABI  0x0001u

/* Identify the running bitstream from ZZ_CAPTURE_FIELD_DIAG_CAP_REG,
 * ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG and ZZ_CAPTURE_CAP_REG
 * (zz9000_capture_calibration.h). The a500 and a500plus bitstreams have
 * identical logic, so both report FWREL_VARIANT_A500.
 * Returns FWREL_VARIANT_UNKNOWN when the registers do not identify it. */
int fwrel_variant_detect(uint32_t diag_cap, uint32_t diag_variant,
                         uint32_t capture_cap);

/* 1 when installing `chosen` on a card running `detected` keeps the same
 * hardware build (a500 and a500plus are interchangeable). */
int fwrel_variant_matches(int detected, int chosen);

/* "zz9000-firmware-<tag>-<variant>.zip". Returns 1 when it fits. */
int fwrel_asset_name(const char *tag, int variant, char *out, size_t size);

/* --- GitHub release JSON ------------------------------------------------- */

/* Copy the string value of top-level `key` from a JSON object. Returns 1
 * when found and it fits (escapes other than \" and \\ are rejected). */
int fwrel_json_top_string(const char *json, size_t len, const char *key,
                          char *out, size_t size);

/* 1 when `value` occurs as a complete JSON string anywhere in the text
 * (used to see whether the release lists an asset name). */
int fwrel_json_has_string(const char *json, size_t len, const char *value);

/* --- HTTP ---------------------------------------------------------------- */

#define FWREL_HOST_MAX 128
#define FWREL_PATH_MAX 1024

/* Split "https://host/path" into host and path ("/" when absent). Ports,
 * user info and other schemes are rejected. Returns 1 on success. */
int fwrel_split_url(const char *url, char *host, size_t host_size,
                    char *path, size_t path_size);

struct fwrel_http_head {
    int status;              /* e.g. 200, 302 */
    long content_length;     /* -1 when absent */
    int chunked;             /* Transfer-Encoding: chunked */
    char location[FWREL_HOST_MAX + FWREL_PATH_MAX + 16];
};

/* Parse a complete response head (status line and headers, without the
 * final blank line). Returns 1 on success. */
int fwrel_http_parse_head(const char *head, size_t len,
                          struct fwrel_http_head *out);

/* Byte sink shared by the chunked decoder and the ZIP extractor; returns
 * 0 on success, nonzero to abort. */
typedef int (*fwrel_sink_fn)(void *ctx, const uint8_t *data, size_t len);

/* Incremental Transfer-Encoding: chunked decoder. */
struct fwrel_chunked {
    int state;
    unsigned long remaining;
    int digits;
};

void fwrel_chunked_init(struct fwrel_chunked *c);
/* Returns FWREL_CHUNKED_MORE, FWREL_CHUNKED_DONE (terminal chunk seen) or
 * FWREL_CHUNKED_ERROR (malformed framing or the sink failed). */
int fwrel_chunked_feed(struct fwrel_chunked *c, const uint8_t *data,
                       size_t len, fwrel_sink_fn sink, void *ctx);
#define FWREL_CHUNKED_MORE   0
#define FWREL_CHUNKED_DONE   1
#define FWREL_CHUNKED_ERROR -1

/* --- Release ZIP extraction ---------------------------------------------- */

/* Extracted image bounds and the Zynq boot header it must carry. */
#define FWREL_BOOT_MIN_BYTES   65536UL
#define FWREL_BOOT_MAX_BYTES   (32UL * 1024UL * 1024UL)
#define FWREL_BOOT_HEADER_BYTES 0x28u
#define FWREL_MODULE_HEADER_BYTES 128u

enum fwrel_zip_result {
    FWREL_ZIP_MORE = 0,       /* feed more input */
    FWREL_ZIP_DONE = 1,       /* selected entries extracted and verified */
    FWREL_ZIP_ERR_FORMAT = -1,
    FWREL_ZIP_ERR_UNSUPPORTED = -2,
    FWREL_ZIP_ERR_NOT_FOUND = -3,
    FWREL_ZIP_ERR_INFLATE = -4,
    FWREL_ZIP_ERR_CRC = -5,
    FWREL_ZIP_ERR_SIZE = -6,
    FWREL_ZIP_ERR_HEADER = -7,
    FWREL_ZIP_ERR_WRITE = -8,
    FWREL_ZIP_ERR_MODULE = -9
};

/* Streaming release-ZIP readers use the vendored miniz tinfl inflater.
 * The legacy reader stops after its verified BOOT.bin; the bundle reader
 * runs through the archive's central directory. */
struct fwrel_zip;

/* NULL when out of memory. This legacy single-image reader accepts BOOT.bin
 * at the archive root or under one directory. */
struct fwrel_zip *fwrel_zip_new(fwrel_sink_fn sink, void *ctx);

/* The bundle reader passes each selected member's safe relative destination
 * ("BOOT.bin" or "ZZ9K/NAME.ZZM") to `begin` before its bytes reach `sink`.
 * `end` follows successful ZIP integrity checks and image validation. Module
 * validation requires the fixed 128-byte ZZM1 little-endian header, a
 * matching exact file length, zero reserved bytes, and SHA-256 of its image.
 * A nonzero callback result aborts with FWREL_ZIP_ERR_WRITE. `begin` and
 * `end` may be NULL. Callers can keep per-member state between these
 * callbacks and the sink. */
typedef int (*fwrel_zip_entry_fn)(void *ctx, const char *path);

/* Extract only dir/BOOT.bin and direct dir/ZZ9K/NAME.ZZM members from one
 * top-level release directory. NAME is 1..8 uppercase [A-Z0-9_]. Invalid,
 * traversing, or deeper paths are rejected. BOOT.bin is required; CFG and
 * other direct files are ignored. */
struct fwrel_zip *fwrel_zip_new_bundle(fwrel_zip_entry_fn begin,
                                       fwrel_sink_fn sink,
                                       fwrel_zip_entry_fn end, void *ctx);

/* Returns an fwrel_zip_result; DONE and errors are sticky. */
int fwrel_zip_feed(struct fwrel_zip *z, const uint8_t *data, size_t len);
/* Release the reader and its inflate state; NULL is ignored. */
void fwrel_zip_free(struct fwrel_zip *z);
/* Bytes of the selected entry written to the sink so far. */
unsigned long fwrel_zip_written(const struct fwrel_zip *z);
const char *fwrel_zip_strerror(int result);

/* 1 when the first bytes carry the Zynq boot-image width word and "XLNX". */
int fwrel_boot_header_ok(const uint8_t *head, size_t len);

#endif /* FW_RELEASE_H */
