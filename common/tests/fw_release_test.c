/*
 * Host unit tests for the online firmware update helpers.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Release ZIPs are built here with zlib in the layout the firmware
 * release script produces (a directory entry, BOOT.bin, ZZ9000.CFG), then
 * fed to the extractor in pieces of every size from one byte upwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "fw_release.h"
#include "zz9000_capture_calibration.h"

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) { g_fails++; printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
    } while (0)

/* ---- versions ---- */

static void test_parse_tag(void) {
    struct fwrel_version v;

    printf("test_parse_tag\n");
    CHECK(fwrel_parse_tag("v2.8.1", &v) && v.major == 2 && v.minor == 8 &&
          v.patch == 1, "v2.8.1 parsed");
    CHECK(fwrel_parse_tag("v10.0.255", &v) && v.major == 10 && v.patch == 255,
          "multi-digit parts parsed");
    CHECK(!fwrel_parse_tag("v2.8.0-rc3", &v), "pre-release suffix rejected");
    CHECK(!fwrel_parse_tag("2.8.1", &v), "missing v rejected");
    CHECK(!fwrel_parse_tag("v2.8", &v), "two parts rejected");
    CHECK(!fwrel_parse_tag("v2.8.1.0", &v), "four parts rejected");
    CHECK(!fwrel_parse_tag("v2.256.0", &v), "part above 255 rejected");
    CHECK(!fwrel_parse_tag("v2..1", &v), "empty part rejected");
}

static void test_compare(void) {
    struct fwrel_version installed = { 2, 8, 0 };
    struct fwrel_version latest = { 2, 8, 1 };

    printf("test_compare\n");
    CHECK(fwrel_compare(&installed, 1, &latest) == FWREL_NEWER,
          "known patch below latest is an update");
    installed.patch = 1;
    CHECK(fwrel_compare(&installed, 1, &latest) == FWREL_UP_TO_DATE,
          "same version is up to date");
    installed.patch = 2;
    CHECK(fwrel_compare(&installed, 1, &latest) == FWREL_INSTALLED_NEWER,
          "development build ahead of release");
    /* Firmware without the patch register reports 2.8 for 2.8.0 and 2.8.1. */
    installed.patch = 0;
    CHECK(fwrel_compare(&installed, 0, &latest) == FWREL_PATCH_UNKNOWN,
          "unknown patch with a patch release is undecided");
    latest.patch = 0;
    CHECK(fwrel_compare(&installed, 0, &latest) == FWREL_UP_TO_DATE,
          "unknown patch against X.Y.0 is up to date");
    latest.minor = 9;
    CHECK(fwrel_compare(&installed, 0, &latest) == FWREL_NEWER,
          "newer minor wins regardless of patch");
    latest.major = 2; latest.minor = 2;
    CHECK(fwrel_compare(&installed, 0, &latest) == FWREL_INSTALLED_NEWER,
          "older release is not offered");
    installed.major = 3; installed.minor = 0;
    latest.major = 2; latest.minor = 255;
    CHECK(fwrel_compare(&installed, 1, &latest) == FWREL_INSTALLED_NEWER,
          "major dominates minor");
}

/* ---- variants ---- */

/* mntzorro.v VCAP_DIAG_VARIANT_VALUE for each release build. */
static uint32_t variant_word(int z3, int fastram, int two_mb, unsigned rgb) {
    unsigned fullrate = z3 ? 1u : (rgb == 1 ? 0u : 1u);
    unsigned csync = rgb == 1;
    return 0x00010000UL | (1u << 7) | (rgb << 5) | (csync << 4) |
           (fullrate << 3) | ((unsigned)two_mb << 2) |
           ((unsigned)fastram << 1) | (unsigned)z3;
}

static void test_variant_detect(void) {
    const uint32_t cap = ZZ_CAPTURE_FIELD_DIAG_CAP;

    printf("test_variant_detect\n");
    CHECK(fwrel_variant_detect(cap, variant_word(1, 1, 0, 0),
                               ZZ_CAPTURE_CAP_E7M) == FWREL_VARIANT_ZORRO3,
          "zorro3");
    CHECK(fwrel_variant_detect(cap, variant_word(1, 1, 0, 0),
                               ZZ_CAPTURE_CAP_C28) == FWREL_VARIANT_ZORRO3_AGA,
          "zorro3-aga from the C28 capture engine");
    CHECK(fwrel_variant_detect(cap, variant_word(0, 0, 0, 2), 0) ==
          FWREL_VARIANT_ZORRO2, "zorro2");
    CHECK(fwrel_variant_detect(cap, variant_word(0, 0, 1, 2), 0) ==
          FWREL_VARIANT_ZORRO2_2MB, "zorro2-2mb");
    CHECK(fwrel_variant_detect(cap, variant_word(0, 0, 0, 1), 0) ==
          FWREL_VARIANT_A500, "a500 (and a500plus, same logic)");
    CHECK(fwrel_variant_detect(cap, variant_word(0, 0, 1, 1), 0) ==
          FWREL_VARIANT_A500_2MB, "a500-2mb");
    CHECK(fwrel_variant_detect(0, variant_word(1, 1, 0, 0), 0) ==
          FWREL_VARIANT_UNKNOWN, "missing diagnostic ABI");
    CHECK(fwrel_variant_detect(cap, variant_word(1, 1, 0, 0) + 0x10000UL, 0) ==
          FWREL_VARIANT_UNKNOWN, "unknown variant-word ABI");
    CHECK(fwrel_variant_detect(cap, variant_word(0, 0, 0, 3), 0) ==
          FWREL_VARIANT_UNKNOWN, "reserved RGB mode");

    CHECK(fwrel_variant_matches(FWREL_VARIANT_A500, FWREL_VARIANT_A500PLUS),
          "a500 and a500plus interchangeable");
    CHECK(!fwrel_variant_matches(FWREL_VARIANT_ZORRO3, FWREL_VARIANT_ZORRO3_AGA),
          "zorro3 and zorro3-aga differ");
    CHECK(!fwrel_variant_matches(FWREL_VARIANT_ZORRO2, FWREL_VARIANT_ZORRO2_2MB),
          "address-space size differs");
    CHECK(fwrel_variant_is_zorro3(FWREL_VARIANT_ZORRO3_AGA) &&
          !fwrel_variant_is_zorro3(FWREL_VARIANT_A500PLUS), "bus classes");
}

static void test_asset_name(void) {
    char name[64];

    printf("test_asset_name\n");
    CHECK(fwrel_asset_name("v2.8.1", FWREL_VARIANT_ZORRO3_AGA, name, sizeof(name)) &&
          strcmp(name, "zz9000-firmware-v2.8.1-zorro3-aga.zip") == 0,
          "release asset naming");
    CHECK(!fwrel_asset_name("v2.8.1", FWREL_VARIANT_COUNT, name, sizeof(name)),
          "invalid variant");
    CHECK(!fwrel_asset_name("v2.8.1", FWREL_VARIANT_ZORRO3, name, 20),
          "truncation reported");
}

/* ---- release JSON ---- */

static void test_json(void) {
    /* Shape of api.github.com/repos/.../releases/latest, with release
     * notes that quote the key and an asset name. */
    static const char json[] =
        "{\"url\":\"https://api.github.com/x\",\"assets\":[{\"name\":"
        "\"zz9000-firmware-v2.8.1-zorro3.zip\",\"tag_name\":\"nested\","
        "\"browser_download_url\":\"https://github.com/BlitterStudio/"
        "zz9000-firmware/releases/download/v2.8.1/zz9000-firmware-v2.8.1-"
        "zorro2.zip\"}],\"body\":\"Use \\\"tag_name\\\": \\\"v9.9.9\\\" "
        "and zz9000-firmware-v2.8.1-a500.zip\",\"tag_name\" : \"v2.8.1\","
        "\"prerelease\":false}";
    char out[32];
    size_t len = sizeof(json) - 1;

    printf("test_json\n");
    CHECK(fwrel_json_top_string(json, len, "tag_name", out, sizeof(out)) &&
          strcmp(out, "v2.8.1") == 0,
          "top-level tag_name, not the nested or quoted ones");
    CHECK(!fwrel_json_top_string(json, len, "missing", out, sizeof(out)),
          "absent key");
    CHECK(!fwrel_json_top_string(json, len, "tag_name", out, 4),
          "value too long for buffer");
    CHECK(fwrel_json_has_string(json, len, "zz9000-firmware-v2.8.1-zorro3.zip"),
          "listed asset found");
    CHECK(!fwrel_json_has_string(json, len, "zz9000-firmware-v2.8.1-zorro2.zip"),
          "name inside a URL is not an asset");
    CHECK(!fwrel_json_has_string(json, len, "zz9000-firmware-v2.8.1-a500.zip"),
          "name inside release notes is not an asset");
    CHECK(!fwrel_json_top_string(json, len - 23, "tag_name", out, sizeof(out)),
          "response cut inside the value");
}

/* ---- HTTP ---- */

static void test_split_url(void) {
    char host[FWREL_HOST_MAX], path[FWREL_PATH_MAX];

    printf("test_split_url\n");
    CHECK(fwrel_split_url("https://api.github.com/repos/a/b/releases/latest",
                          host, sizeof(host), path, sizeof(path)) &&
          strcmp(host, "api.github.com") == 0 &&
          strcmp(path, "/repos/a/b/releases/latest") == 0, "host and path");
    CHECK(fwrel_split_url("https://example.com", host, sizeof(host), path,
                          sizeof(path)) && strcmp(path, "/") == 0,
          "bare host gets /");
    CHECK(!fwrel_split_url("http://github.com/", host, sizeof(host), path,
                           sizeof(path)), "plain http refused");
    CHECK(!fwrel_split_url("https://github.com:8443/", host, sizeof(host),
                           path, sizeof(path)), "explicit port refused");
    CHECK(!fwrel_split_url("https://user@github.com/", host, sizeof(host),
                           path, sizeof(path)), "user info refused");
    CHECK(!fwrel_split_url("https://github.com/a b", host, sizeof(host), path,
                           sizeof(path)), "space in path refused");
}

static void test_http_head(void) {
    static const char redirect[] =
        "HTTP/1.1 302 Found\r\nServer: GitHub.com\r\n"
        "location: https://release-assets.githubusercontent.com/x?y=1\r\n"
        "Content-Length: 0";
    static const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding:  Chunked \r\n";
    static const char sized[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 1459061\r\n";
    struct fwrel_http_head h;

    printf("test_http_head\n");
    CHECK(fwrel_http_parse_head(redirect, sizeof(redirect) - 1, &h) &&
          h.status == 302 && h.content_length == 0 && !h.chunked &&
          strcmp(h.location,
                 "https://release-assets.githubusercontent.com/x?y=1") == 0,
          "redirect with case-insensitive Location");
    CHECK(fwrel_http_parse_head(chunked, sizeof(chunked) - 1, &h) &&
          h.status == 200 && h.chunked && h.content_length == -1,
          "chunked body");
    CHECK(fwrel_http_parse_head(sized, sizeof(sized) - 1, &h) &&
          h.content_length == 1459061L, "sized body");
    CHECK(!fwrel_http_parse_head("HTTP/2 200\r\n", 12, &h), "not HTTP/1.x");
    CHECK(!fwrel_http_parse_head("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n",
                                 38, &h), "bad length");
    CHECK(!fwrel_http_parse_head("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n",
                                 51, &h), "layered encodings refused");
}

static unsigned char sink_buf[1u << 22];
static size_t sink_len;
static size_t sink_fail_at = (size_t)-1;

static int sink(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (sink_len + len > sizeof(sink_buf) || sink_len + len > sink_fail_at)
        return 1;
    memcpy(sink_buf + sink_len, data, len);
    sink_len += len;
    return 0;
}

static int chunked_run(const char *body, size_t step) {
    struct fwrel_chunked c;
    size_t len = strlen(body), i;
    int rc = FWREL_CHUNKED_MORE;

    fwrel_chunked_init(&c);
    sink_len = 0;
    for (i = 0; i < len && rc == FWREL_CHUNKED_MORE; i += step) {
        size_t n = len - i < step ? len - i : step;
        rc = fwrel_chunked_feed(&c, (const uint8_t *)body + i, n, sink, NULL);
    }
    return rc;
}

static void test_chunked(void) {
    static const char body[] =
        "5\r\nhello\r\n1A;ext=1\r\nabcdefghijklmnopqrstuvwxyz\r\n"
        "0\r\nX-Trailer: 1\r\n\r\n";
    size_t step;
    int all_ok = 1;

    printf("test_chunked\n");
    for (step = 1; step <= sizeof(body); step++) {
        if (chunked_run(body, step) != FWREL_CHUNKED_DONE || sink_len != 31 ||
            memcmp(sink_buf, "helloabcdefghijklmnopqrstuvwxyz", 31) != 0)
            all_ok = 0;
    }
    CHECK(all_ok, "decoded identically at every split");
    CHECK(chunked_run("5\r\nhelloX\r\n0\r\n\r\n", 64) == FWREL_CHUNKED_ERROR,
          "missing CRLF after data");
    CHECK(chunked_run("zz\r\n", 64) == FWREL_CHUNKED_ERROR, "bad size");
    CHECK(chunked_run("5\r\nhel", 64) == FWREL_CHUNKED_MORE, "incomplete");
    CHECK(chunked_run("FFFFFFFF\r\n", 64) == FWREL_CHUNKED_ERROR,
          "oversized chunk refused");
}

/* ---- release ZIP ---- */

static unsigned char zip_buf[1u << 22];
static size_t zip_len;

static void put16(unsigned v) {
    zip_buf[zip_len++] = (unsigned char)v;
    zip_buf[zip_len++] = (unsigned char)(v >> 8);
}

static void put32(unsigned long v) {
    put16((unsigned)(v & 0xffffu));
    put16((unsigned)(v >> 16));
}

enum { ENTRY_STORE, ENTRY_DEFLATE, ENTRY_DEFLATE_DESCRIPTOR };

/* Append one local entry; a nonzero crc_override is recorded instead. */
static void add_entry(const char *name, const unsigned char *data, size_t len,
                      int kind, unsigned long crc_override) {
    unsigned long crc = crc32(0, data, (uInt)len);
    size_t header = zip_len, csize;
    int descriptor = kind == ENTRY_DEFLATE_DESCRIPTOR;

    if (crc_override) crc = crc_override;
    put32(0x04034b50UL);
    put16(20);
    put16(descriptor ? 8 : 0);
    put16(kind == ENTRY_STORE ? 0 : 8);
    put16(0); put16(0);
    put32(descriptor ? 0 : crc);
    put32(0); /* csize patched below */
    put32(descriptor ? 0 : (unsigned long)len);
    put16((unsigned)strlen(name));
    put16(4);
    memcpy(zip_buf + zip_len, name, strlen(name));
    zip_len += strlen(name);
    put32(0xcafe0000UL); /* an extra field the reader must skip */
    if (kind == ENTRY_STORE) {
        memcpy(zip_buf + zip_len, data, len);
        csize = len;
    } else {
        z_stream z;
        memset(&z, 0, sizeof(z));
        deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        z.next_in = (Bytef *)data;
        z.avail_in = (uInt)len;
        z.next_out = zip_buf + zip_len;
        z.avail_out = (uInt)(sizeof(zip_buf) - zip_len);
        deflate(&z, Z_FINISH);
        csize = z.total_out;
        deflateEnd(&z);
    }
    zip_len += csize;
    if (!descriptor) {
        size_t save = zip_len;
        zip_len = header + 18;
        put32((unsigned long)csize);
        zip_len = save;
    } else {
        put32(0x08074b50UL);
        put32(crc);
        put32((unsigned long)csize);
        put32((unsigned long)len);
    }
}

static void end_zip(void) {
    put32(0x02014b50UL); /* central directory start; contents irrelevant */
    memset(zip_buf + zip_len, 0, 42);
    zip_len += 42;
}

static unsigned char boot[300000];

static void make_boot(void) {
    size_t i;
    unsigned long x = 12345;

    for (i = 0; i < sizeof(boot); i++) {
        x = x * 1103515245UL + 12345UL;
        /* Compressible but not trivial. */
        boot[i] = (unsigned char)((i & 0x3ff) < 512 ? (x >> 16) : (i >> 3));
    }
    memcpy(boot + 0x20, "\x66\x55\x99\xaa" "XNLX", 8);
}

static const char cfg[] = "videocap_profile = filtered_60\n";

static unsigned char module[FWREL_MODULE_HEADER_BYTES + 12];

static void put_le16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_le32(unsigned char *p, unsigned long value) {
    put_le16(p, (unsigned)(value & 0xffffu));
    put_le16(p + 2, (unsigned)(value >> 16));
}

static void make_module(void) {
    static const unsigned char digest[32] = {
        0xcd, 0x67, 0xb6, 0x48, 0x17, 0xd6, 0x34, 0x25,
        0xee, 0xdf, 0x4c, 0x6b, 0xfa, 0xeb, 0x2b, 0xcb,
        0xbd, 0xc1, 0x53, 0xcc, 0xd3, 0xeb, 0x6f, 0x9a,
        0xce, 0x2e, 0x26, 0x42, 0xc1, 0x0b, 0x9e, 0x5d
    };

    memset(module, 0, sizeof(module));
    memcpy(module, "ZZM1", 4);
    put_le16(module + 4, FWREL_MODULE_HEADER_BYTES);
    put_le16(module + 6, 1);
    put_le32(module + 24, 12);
    memcpy(module + 36, "VIDEO", 6);
    memcpy(module + 52, digest, sizeof(digest));
    memcpy(module + FWREL_MODULE_HEADER_BYTES, "module image", 12);
}

static void build_release_zip(int boot_kind, unsigned long crc_override) {
    zip_len = 0;
    add_entry("zz9000-firmware-v2.8.1-zorro3/", (const unsigned char *)"", 0,
              ENTRY_STORE, 0);
    add_entry("zz9000-firmware-v2.8.1-zorro3/BOOT.bin", boot, sizeof(boot),
              boot_kind, crc_override);
    add_entry("zz9000-firmware-v2.8.1-zorro3/ZZ9000.CFG",
              (const unsigned char *)cfg, sizeof(cfg) - 1, ENTRY_DEFLATE, 0);
    end_zip();
}

static char bundle_started[4][20];
static char bundle_finished[4][20];
static size_t bundle_start_count;
static size_t bundle_finish_count;

static int bundle_begin(void *ctx, const char *path) {
    (void)ctx;
    if (bundle_start_count >= sizeof(bundle_started) / sizeof(bundle_started[0]))
        return 1;
    strcpy(bundle_started[bundle_start_count++], path);
    return 0;
}

static int bundle_end(void *ctx, const char *path) {
    (void)ctx;
    if (bundle_finish_count >= sizeof(bundle_finished) / sizeof(bundle_finished[0]))
        return 1;
    strcpy(bundle_finished[bundle_finish_count++], path);
    return 0;
}

static void build_bundle_zip(void) {
    zip_len = 0;
    add_entry("zz9000-firmware-v2.8.1-zorro3/", (const unsigned char *)"", 0,
              ENTRY_STORE, 0);
    add_entry("zz9000-firmware-v2.8.1-zorro3/ZZ9K/",
              (const unsigned char *)"", 0, ENTRY_STORE, 0);
    add_entry("zz9000-firmware-v2.8.1-zorro3/ZZ9K/VIDEO.ZZM", module,
              sizeof(module), ENTRY_DEFLATE, 0);
    add_entry("zz9000-firmware-v2.8.1-zorro3/BOOT.bin", boot, sizeof(boot),
              ENTRY_DEFLATE, 0);
    add_entry("zz9000-firmware-v2.8.1-zorro3/ZZ9000.CFG",
              (const unsigned char *)cfg, sizeof(cfg) - 1, ENTRY_DEFLATE, 0);
    end_zip();
}

static int bundle_run(size_t step) {
    struct fwrel_zip *z = fwrel_zip_new_bundle(bundle_begin, sink, bundle_end,
                                                NULL);
    size_t i;
    int rc = FWREL_ZIP_MORE;

    sink_len = 0;
    bundle_start_count = 0;
    bundle_finish_count = 0;
    for (i = 0; i < zip_len && rc == FWREL_ZIP_MORE; i += step) {
        size_t n = zip_len - i < step ? zip_len - i : step;
        rc = fwrel_zip_feed(z, zip_buf + i, n);
    }
    fwrel_zip_free(z);
    return rc;
}

/* Feed zip_buf in `step`-byte pieces; returns the final result. */
static int zip_run(size_t step) {
    struct fwrel_zip *z = fwrel_zip_new(sink, NULL);
    size_t i;
    int rc = FWREL_ZIP_MORE;

    sink_len = 0;
    for (i = 0; i < zip_len && rc == FWREL_ZIP_MORE; i += step) {
        size_t n = zip_len - i < step ? zip_len - i : step;
        rc = fwrel_zip_feed(z, zip_buf + i, n);
    }
    fwrel_zip_free(z);
    return rc;
}

static int extracted_ok(void) {
    return sink_len == sizeof(boot) && memcmp(sink_buf, boot, sizeof(boot)) == 0;
}

static void test_zip_extract(void) {
    static const size_t steps[] = { 1, 2, 3, 29, 30, 31, 4096, 65536, 1u << 22 };
    size_t s;
    int ok;

    printf("test_zip_extract\n");
    make_boot();
    build_release_zip(ENTRY_DEFLATE, 0);
    ok = 1;
    for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
        if (zip_run(steps[s]) != FWREL_ZIP_DONE || !extracted_ok())
            ok = 0;
    CHECK(ok, "deflated BOOT.bin extracted at every split");

    build_release_zip(ENTRY_DEFLATE_DESCRIPTOR, 0);
    ok = 1;
    for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
        if (zip_run(steps[s]) != FWREL_ZIP_DONE || !extracted_ok())
            ok = 0;
    CHECK(ok, "streamed entry with a data descriptor");

    build_release_zip(ENTRY_STORE, 0);
    CHECK(zip_run(7) == FWREL_ZIP_DONE && extracted_ok(), "stored BOOT.bin");

    build_release_zip(ENTRY_DEFLATE, 0xdeadbeefUL);
    CHECK(zip_run(4096) == FWREL_ZIP_ERR_CRC, "CRC mismatch detected");
    build_release_zip(ENTRY_DEFLATE_DESCRIPTOR, 0xdeadbeefUL);
    CHECK(zip_run(4096) == FWREL_ZIP_ERR_CRC, "descriptor CRC mismatch detected");

    /* Flip one compressed byte in BOOT.bin's data. */
    build_release_zip(ENTRY_DEFLATE, 0);
    zip_buf[30 + 31 + 4 + 30 + 39 + 4 + 2000] ^= 0x55;
    {
        int rc = zip_run(4096);
        CHECK(rc == FWREL_ZIP_ERR_CRC || rc == FWREL_ZIP_ERR_INFLATE ||
              rc == FWREL_ZIP_ERR_SIZE, "corrupt data rejected");
    }

    build_release_zip(ENTRY_DEFLATE, 0);
    CHECK(zip_run(100) == FWREL_ZIP_DONE, "baseline");
    zip_len -= 50000; /* connection dropped mid-file */
    CHECK(zip_run(100) == FWREL_ZIP_MORE, "truncated download is incomplete");

    sink_fail_at = 100000;
    build_release_zip(ENTRY_DEFLATE, 0);
    CHECK(zip_run(4096) == FWREL_ZIP_ERR_WRITE, "write failure reported");
    sink_fail_at = (size_t)-1;
}

static void test_bundle_extract(void) {
    printf("test_bundle_extract\n");
    make_boot();
    make_module();
    build_bundle_zip();
    CHECK(bundle_run(29) == FWREL_ZIP_DONE &&
          bundle_start_count == 2 && bundle_finish_count == 2 &&
          strcmp(bundle_started[0], "ZZ9K/VIDEO.ZZM") == 0 &&
          strcmp(bundle_started[1], "BOOT.bin") == 0 &&
          strcmp(bundle_finished[0], "ZZ9K/VIDEO.ZZM") == 0 &&
          strcmp(bundle_finished[1], "BOOT.bin") == 0 &&
          sink_len == sizeof(module) + sizeof(boot) &&
          memcmp(sink_buf, module, sizeof(module)) == 0 &&
          memcmp(sink_buf + sizeof(module), boot, sizeof(boot)) == 0,
          "bundle selects only verified modules and BOOT");

    module[52] ^= 1;
    build_bundle_zip();
    CHECK(bundle_run(4096) == FWREL_ZIP_ERR_MODULE, "module SHA-256 checked");
    make_module();

    zip_len = 0;
    add_entry("release/BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    add_entry("release/ZZ9K/BAD-NAME.ZZM", module, sizeof(module),
              ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(bundle_run(4096) == FWREL_ZIP_ERR_FORMAT, "invalid module name rejected");

    zip_len = 0;
    add_entry("release/ZZ9K/deep/VIDEO.ZZM", module, sizeof(module),
              ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(bundle_run(4096) == FWREL_ZIP_ERR_FORMAT, "deep module path rejected");

    zip_len = 0;
    add_entry("release/../BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(bundle_run(4096) == FWREL_ZIP_ERR_FORMAT, "traversal rejected");
}

static void test_zip_rejects(void) {
    printf("test_zip_rejects\n");

    zip_len = 0;
    add_entry("zz9000-firmware-v2.8.1-zorro3/ZZ9000.CFG",
              (const unsigned char *)cfg, sizeof(cfg) - 1, ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(zip_run(64) == FWREL_ZIP_ERR_NOT_FOUND, "no BOOT.bin");

    zip_len = 0;
    add_entry("a/b/BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(zip_run(64) == FWREL_ZIP_ERR_NOT_FOUND, "BOOT.bin nested too deep");

    zip_len = 0;
    add_entry("BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(zip_run(64) == FWREL_ZIP_DONE && extracted_ok(), "BOOT.bin at the root");

    memcpy(boot + 0x24, "ABCD", 4);
    zip_len = 0;
    add_entry("BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(zip_run(64) == FWREL_ZIP_ERR_HEADER, "not a Zynq boot image");
    make_boot();

    zip_len = 0;
    add_entry("BOOT.bin", boot, 4096, ENTRY_DEFLATE, 0);
    end_zip();
    CHECK(zip_run(64) == FWREL_ZIP_ERR_SIZE, "implausibly small image");

    memcpy(zip_buf, "<!DOCTYPE html>", 15);
    zip_len = 15;
    CHECK(zip_run(64) == FWREL_ZIP_ERR_FORMAT, "HTML error page");

    zip_len = 0;
    add_entry("BOOT.bin", boot, sizeof(boot), ENTRY_DEFLATE, 0);
    zip_buf[6] = 1; /* encrypted flag */
    CHECK(zip_run(64) == FWREL_ZIP_ERR_UNSUPPORTED, "encrypted entry");
}

int main(void) {
    test_parse_tag();
    test_compare();
    test_variant_detect();
    test_asset_name();
    test_json();
    test_split_url();
    test_http_head();
    test_chunked();
    test_zip_extract();
    test_bundle_extract();
    test_zip_rejects();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
