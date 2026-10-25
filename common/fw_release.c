/*
 * Portable helpers for the online firmware update (see fw_release.h).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz/miniz.h"

#include "fw_release.h"
#include "zz9000_capture_calibration.h"

/* --- Versions ----------------------------------------------------------- */

/* Parse one decimal component 0..255 ending at `end` ('.' or NUL). */
static const char *parse_part(const char *s, char end, unsigned *out)
{
    unsigned value = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (unsigned)(*s - '0');
        if (++digits > 3 || value > 255u)
            return NULL;
        s++;
    }
    if (!digits || *s != end)
        return NULL;
    *out = value;
    return end ? s + 1 : s;
}

int fwrel_parse_tag(const char *tag, struct fwrel_version *out)
{
    struct fwrel_version v;

    if (!tag || tag[0] != 'v')
        return 0;
    tag = parse_part(tag + 1, '.', &v.major);
    if (tag) tag = parse_part(tag, '.', &v.minor);
    if (tag) tag = parse_part(tag, '\0', &v.patch);
    if (!tag)
        return 0;
    *out = v;
    return 1;
}

enum fwrel_compare fwrel_compare(const struct fwrel_version *installed,
                                 int patch_known,
                                 const struct fwrel_version *latest)
{
    unsigned long have = ((unsigned long)installed->major << 8) | installed->minor;
    unsigned long want = ((unsigned long)latest->major << 8) | latest->minor;

    if (want > have)
        return FWREL_NEWER;
    if (want < have)
        return FWREL_INSTALLED_NEWER;
    if (!patch_known)
        return latest->patch == 0 ? FWREL_UP_TO_DATE : FWREL_PATCH_UNKNOWN;
    if (latest->patch > installed->patch)
        return FWREL_NEWER;
    if (latest->patch < installed->patch)
        return FWREL_INSTALLED_NEWER;
    return FWREL_UP_TO_DATE;
}

/* --- Board variants ------------------------------------------------------ */

static const char *const variant_names[FWREL_VARIANT_COUNT] = {
    "zorro3", "zorro3-aga", "zorro2", "zorro2-2mb", "a500", "a500-2mb",
    "a500plus"
};

static const char *const variant_labels[FWREL_VARIANT_COUNT] = {
    "Zorro III",
    "Zorro III, A4000 AGA capture",
    "Zorro II",
    "Zorro II, 2 MB",
    "A500 (ZZ9500CX)",
    "A500 (ZZ9500CX), 2 MB",
    "A500+ / Super Denise"
};

const char *fwrel_variant_name(int variant)
{
    if (variant < 0 || variant >= FWREL_VARIANT_COUNT)
        return NULL;
    return variant_names[variant];
}

const char *fwrel_variant_label(int variant)
{
    if (variant < 0 || variant >= FWREL_VARIANT_COUNT)
        return NULL;
    return variant_labels[variant];
}

int fwrel_variant_is_zorro3(int variant)
{
    return variant == FWREL_VARIANT_ZORRO3 ||
           variant == FWREL_VARIANT_ZORRO3_AGA;
}

/* Variant word (mntzorro.v VCAP_DIAG_VARIANT_VALUE): bits 31..16 ABI,
 * bit 0 Zorro III, bit 2 2 MB address space, bits 6..5 capture RGB mode
 * (0 video slot Zorro III, 1 Denise adapter, 2 Zorro II video slot). */
int fwrel_variant_detect(uint32_t diag_cap, uint32_t diag_variant,
                         uint32_t capture_cap)
{
    int z3, two_mb;
    unsigned rgb_mode;

    if (diag_cap != ZZ_CAPTURE_FIELD_DIAG_CAP ||
        (diag_variant >> 16) != FWREL_DIAG_VARIANT_ABI)
        return FWREL_VARIANT_UNKNOWN;
    z3 = (diag_variant & 0x1u) != 0;
    two_mb = (diag_variant & 0x4u) != 0;
    rgb_mode = (unsigned)((diag_variant >> 5) & 0x3u);
    if (z3) {
        if (two_mb || rgb_mode != 0)
            return FWREL_VARIANT_UNKNOWN;
        return capture_cap == ZZ_CAPTURE_CAP_C28 ?
            FWREL_VARIANT_ZORRO3_AGA : FWREL_VARIANT_ZORRO3;
    }
    if (rgb_mode == 1)
        return two_mb ? FWREL_VARIANT_A500_2MB : FWREL_VARIANT_A500;
    if (rgb_mode == 2)
        return two_mb ? FWREL_VARIANT_ZORRO2_2MB : FWREL_VARIANT_ZORRO2;
    return FWREL_VARIANT_UNKNOWN;
}

int fwrel_variant_matches(int detected, int chosen)
{
    if (detected == FWREL_VARIANT_A500 && chosen == FWREL_VARIANT_A500PLUS)
        return 1;
    if (detected == FWREL_VARIANT_A500PLUS && chosen == FWREL_VARIANT_A500)
        return 1;
    return detected == chosen;
}

int fwrel_asset_name(const char *tag, int variant, char *out, size_t size)
{
    const char *name = fwrel_variant_name(variant);
    int n;

    if (!name || !tag || !size)
        return 0;
    n = snprintf(out, size, "zz9000-firmware-%s-%s.zip", tag, name);
    return n > 0 && (size_t)n < size;
}

/* --- GitHub release JSON ------------------------------------------------- */

/* Scan one JSON string starting at the opening quote. On return *pos is
 * just past the closing quote. When `out` is set the decoded value is
 * stored there (only \" \\ and \/ escapes); *ok becomes 0 on overflow or
 * any other escape. Returns 0 on unterminated input. */
static int scan_string(const char *json, size_t len, size_t *pos,
                       char *out, size_t size, int *ok)
{
    size_t i = *pos + 1;
    size_t n = 0;

    while (i < len) {
        char c = json[i++];
        if (c == '"') {
            if (out) {
                if (n < size) out[n] = '\0';
                else *ok = 0;
            }
            *pos = i;
            return 1;
        }
        if (c == '\\') {
            if (i >= len)
                return 0;
            c = json[i++];
            if (c != '"' && c != '\\' && c != '/')
                *ok = 0;
        }
        if (out) {
            if (n + 1 < size) out[n++] = c;
            else *ok = 0;
        }
    }
    return 0;
}

int fwrel_json_top_string(const char *json, size_t len, const char *key,
                          char *out, size_t size)
{
    size_t i = 0;
    int depth = 0;
    size_t key_len = strlen(key);

    while (i < len) {
        char c = json[i];
        if (c == '{' || c == '[') {
            depth++;
            i++;
        } else if (c == '}' || c == ']') {
            depth--;
            i++;
        } else if (c == '"') {
            size_t start = i + 1;
            int ok = 1;
            int is_key;
            size_t j;

            if (!scan_string(json, len, &i, NULL, 0, &ok))
                return 0;
            /* A string followed by ':' is a key. */
            j = i;
            while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                               json[j] == '\r' || json[j] == '\n'))
                j++;
            is_key = j < len && json[j] == ':';
            if (!is_key || depth != 1 || i - start - 1 != key_len ||
                memcmp(json + start, key, key_len) != 0)
                continue;
            j++;
            while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                               json[j] == '\r' || json[j] == '\n'))
                j++;
            if (j >= len || json[j] != '"')
                return 0;
            if (!scan_string(json, len, &j, out, size, &ok))
                return 0;
            return ok;
        } else {
            i++;
        }
    }
    return 0;
}

int fwrel_json_has_string(const char *json, size_t len, const char *value)
{
    size_t i = 0;
    size_t value_len = strlen(value);

    while (i < len) {
        if (json[i] == '"') {
            size_t start = i + 1;
            int ok = 1;
            if (!scan_string(json, len, &i, NULL, 0, &ok))
                return 0;
            if (i - start - 1 == value_len &&
                memcmp(json + start, value, value_len) == 0)
                return 1;
        } else {
            i++;
        }
    }
    return 0;
}

/* --- HTTP ---------------------------------------------------------------- */

int fwrel_split_url(const char *url, char *host, size_t host_size,
                    char *path, size_t path_size)
{
    static const char scheme[] = "https://";
    const char *h, *slash;
    size_t host_len, i;

    if (!url || strncmp(url, scheme, sizeof(scheme) - 1) != 0)
        return 0;
    h = url + sizeof(scheme) - 1;
    slash = strchr(h, '/');
    host_len = slash ? (size_t)(slash - h) : strlen(h);
    if (host_len == 0 || host_len >= host_size)
        return 0;
    for (i = 0; i < host_len; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
    }
    memcpy(host, h, host_len);
    host[host_len] = '\0';
    if (!slash)
        slash = "/";
    if (strlen(slash) >= path_size)
        return 0;
    for (i = 0; slash[i]; i++)
        if ((unsigned char)slash[i] <= ' ' || slash[i] == 0x7f)
            return 0;
    strcpy(path, slash);
    return 1;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Case-insensitive match of header name `name` at the start of a line. */
static int header_is(const char *line, size_t len, const char *name)
{
    size_t n = strlen(name);
    size_t i;

    if (len <= n || line[n] != ':')
        return 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)line[i]) != name[i])
            return 0;
    return 1;
}

/* Header value without the name, colon and surrounding blanks. */
static void header_value(const char *line, size_t len, const char *name,
                         const char **value, size_t *value_len)
{
    size_t start = strlen(name) + 1;

    while (start < len && (line[start] == ' ' || line[start] == '\t'))
        start++;
    while (len > start && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    *value = line + start;
    *value_len = len - start;
}

int fwrel_http_parse_head(const char *head, size_t len,
                          struct fwrel_http_head *out)
{
    size_t pos = 0;
    int first = 1;

    out->status = 0;
    out->content_length = -1;
    out->chunked = 0;
    out->location[0] = '\0';

    while (pos < len) {
        size_t end = pos;
        size_t line_len;
        const char *line = head + pos;

        while (end < len && head[end] != '\n')
            end++;
        line_len = end - pos;
        if (line_len && line[line_len - 1] == '\r')
            line_len--;
        if (first) {
            /* "HTTP/1.x NNN reason" */
            if (line_len < 12 || memcmp(line, "HTTP/1.", 7) != 0 ||
                line[8] != ' ' ||
                line[9] < '1' || line[9] > '5' ||
                line[10] < '0' || line[10] > '9' ||
                line[11] < '0' || line[11] > '9')
                return 0;
            out->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 +
                          (line[11] - '0');
            first = 0;
        } else if (header_is(line, line_len, "content-length")) {
            const char *v;
            size_t vl, i;
            long n = 0;

            header_value(line, line_len, "content-length", &v, &vl);
            if (vl == 0 || vl > 9)
                return 0;
            for (i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9')
                    return 0;
                n = n * 10 + (v[i] - '0');
            }
            out->content_length = n;
        } else if (header_is(line, line_len, "transfer-encoding")) {
            const char *v;
            size_t vl, i;
            static const char chunked[] = "chunked";

            header_value(line, line_len, "transfer-encoding", &v, &vl);
            if (vl != sizeof(chunked) - 1)
                return 0; /* only plain chunked is supported */
            for (i = 0; i < vl; i++)
                if (lower((unsigned char)v[i]) != chunked[i])
                    return 0;
            out->chunked = 1;
        } else if (header_is(line, line_len, "location")) {
            const char *v;
            size_t vl;

            header_value(line, line_len, "location", &v, &vl);
            if (vl >= sizeof(out->location))
                return 0;
            memcpy(out->location, v, vl);
            out->location[vl] = '\0';
        }
        pos = end + 1;
    }
    return !first;
}

enum {
    CH_SIZE,      /* hex digits */
    CH_EXT,       /* chunk extension until CR */
    CH_SIZE_LF,
    CH_DATA,
    CH_DATA_CR,
    CH_DATA_LF,
    CH_TRAILER,   /* after the 0 chunk: lines until an empty one */
    CH_TRAILER_LINE,
    CH_TRAILER_LF,
    CH_DONE,
    CH_ERROR
};

void fwrel_chunked_init(struct fwrel_chunked *c)
{
    c->state = CH_SIZE;
    c->remaining = 0;
    c->digits = 0;
}

static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (uint8_t)lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int fwrel_chunked_feed(struct fwrel_chunked *c, const uint8_t *data,
                       size_t len, fwrel_sink_fn sink, void *ctx)
{
    size_t i = 0;

    while (i < len && c->state != CH_DONE && c->state != CH_ERROR) {
        uint8_t b = data[i];
        switch (c->state) {
        case CH_SIZE: {
            int h = hex_value(b);
            if (h >= 0) {
                if (++c->digits > 7) { c->state = CH_ERROR; break; }
                c->remaining = (c->remaining << 4) | (unsigned long)h;
                i++;
            } else if (!c->digits) {
                c->state = CH_ERROR;
            } else if (b == '\r') {
                c->state = CH_SIZE_LF;
                i++;
            } else if (b == ';' || b == ' ' || b == '\t') {
                c->state = CH_EXT;
                i++;
            } else {
                c->state = CH_ERROR;
            }
            break;
        }
        case CH_EXT:
            if (b == '\r') c->state = CH_SIZE_LF;
            i++;
            break;
        case CH_SIZE_LF:
            if (b != '\n') { c->state = CH_ERROR; break; }
            i++;
            c->digits = 0;
            c->state = c->remaining ? CH_DATA : CH_TRAILER;
            break;
        case CH_DATA: {
            size_t n = len - i;
            if (n > c->remaining) n = c->remaining;
            if (sink(ctx, data + i, n)) { c->state = CH_ERROR; break; }
            i += n;
            c->remaining -= n;
            if (!c->remaining) c->state = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            c->state = b == '\r' ? CH_DATA_LF : CH_ERROR;
            i++;
            break;
        case CH_DATA_LF:
            c->state = b == '\n' ? CH_SIZE : CH_ERROR;
            i++;
            break;
        case CH_TRAILER:
            /* Start of a trailer line, or the final CRLF. */
            if (b == '\r') c->state = CH_TRAILER_LF;
            else c->state = CH_TRAILER_LINE;
            i++;
            break;
        case CH_TRAILER_LINE:
            if (b == '\n') c->state = CH_TRAILER;
            i++;
            break;
        case CH_TRAILER_LF:
            c->state = b == '\n' ? CH_DONE : CH_ERROR;
            i++;
            break;
        }
    }
    if (c->state == CH_ERROR) return FWREL_CHUNKED_ERROR;
    if (c->state == CH_DONE) return FWREL_CHUNKED_DONE;
    return FWREL_CHUNKED_MORE;
}

/* --- Release ZIP extraction ---------------------------------------------- */

#define ZIP_LOCAL_SIG      0x04034b50UL
#define ZIP_CENTRAL_SIG    0x02014b50UL
#define ZIP_DESCRIPTOR_SIG 0x08074b50UL
#define ZIP_LOCAL_BYTES    30u
#define ZIP_FLAG_ENCRYPTED 0x0001u
#define ZIP_FLAG_DESCRIPTOR 0x0008u
#define ZIP_METHOD_STORE   0u
#define ZIP_METHOD_DEFLATE 8u

enum {
    ZS_LOCAL,       /* collecting a local header (or central signature) */
    ZS_NAME,        /* collecting name + extra */
    ZS_SKIP,        /* skipping a stored, sized entry */
    ZS_DATA,        /* entry data: stored copy or inflate */
    ZS_DESCRIPTOR,  /* collecting a data descriptor */
    ZS_FINISHED
};

enum {
    ZIP_ENTRY_NONE,
    ZIP_ENTRY_BOOT,
    ZIP_ENTRY_MODULE
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fwrel_boot_header_ok(const uint8_t *head, size_t len)
{
    static const uint8_t width[4] = { 0x66, 0x55, 0x99, 0xaa };
    static const uint8_t magic[4] = { 'X', 'N', 'L', 'X' };

    return len >= FWREL_BOOT_HEADER_BYTES &&
           memcmp(head + 0x20, width, 4) == 0 &&
           memcmp(head + 0x24, magic, 4) == 0;
}

struct fwrel_sha256 {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
};

struct fwrel_zip {
    int state;
    int result;
    fwrel_sink_fn sink;
    fwrel_zip_entry_fn begin;
    fwrel_zip_entry_fn end;
    void *ctx;
    uint8_t hdr[ZIP_LOCAL_BYTES];
    size_t hdr_have;
    size_t hdr_need;
    char name[256];
    char entry_name[sizeof("ZZ9K/12345678.ZZM")];
    char root[256];
    uint16_t flags;
    uint16_t method;
    uint32_t crc_expected;
    uint32_t csize;
    uint32_t usize;
    uint16_t name_len;
    uint16_t extra_len;
    unsigned long skip;
    int bundle;
    int target;
    int found_boot;
    uint32_t crc;
    unsigned long out_total;
    uint8_t head[FWREL_MODULE_HEADER_BYTES];
    struct fwrel_sha256 sha256;
    uint32_t module_image_size;
    int module_header_ok;
    tinfl_decompressor inflater;
    size_t dict_ofs;
    uint8_t dict[TINFL_LZ_DICT_SIZE]; /* inflate window and output */
};

/* CRC-32 (IEEE 802.3, as ZIP and zlib's crc32()): pass 0 to start. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static int ready;

    if (!ready) {
        uint32_t n, k, c;
        for (n = 0; n < 256; n++) {
            c = n;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t sha256_rotr(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static void sha256_block(struct fwrel_sha256 *sha, const uint8_t *data)
{
    static const uint32_t k[64] = {
        0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
        0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
        0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
        0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
        0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
        0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
        0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
        0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
        0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
        0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
        0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
        0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
        0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
        0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
        0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
        0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
    for (; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = sha->state[0]; b = sha->state[1]; c = sha->state[2]; d = sha->state[3];
    e = sha->state[4]; f = sha->state[5]; g = sha->state[6]; h = sha->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choose + k[i] + w[i];
        uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    sha->state[0] += a; sha->state[1] += b; sha->state[2] += c; sha->state[3] += d;
    sha->state[4] += e; sha->state[5] += f; sha->state[6] += g; sha->state[7] += h;
}

static void sha256_init(struct fwrel_sha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
    };

    memcpy(sha->state, initial, sizeof(initial));
    sha->bits = 0;
    sha->used = 0;
}

static void sha256_update(struct fwrel_sha256 *sha, const uint8_t *data, size_t len)
{
    sha->bits += (uint64_t)len * 8u;
    while (len) {
        size_t n = sizeof(sha->block) - sha->used;
        if (n > len) n = len;
        memcpy(sha->block + sha->used, data, n);
        sha->used += n;
        data += n;
        len -= n;
        if (sha->used == sizeof(sha->block)) {
            sha256_block(sha, sha->block);
            sha->used = 0;
        }
    }
}

static void sha256_final(struct fwrel_sha256 *sha, uint8_t out[32])
{
    unsigned i;

    sha->block[sha->used++] = 0x80;
    if (sha->used > 56) {
        memset(sha->block + sha->used, 0, sizeof(sha->block) - sha->used);
        sha256_block(sha, sha->block);
        sha->used = 0;
    }
    memset(sha->block + sha->used, 0, 56 - sha->used);
    for (i = 0; i < 8; i++)
        sha->block[56 + i] = (uint8_t)(sha->bits >> (56 - i * 8));
    sha256_block(sha, sha->block);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(sha->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(sha->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(sha->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)sha->state[i];
    }
}

static struct fwrel_zip *zip_new(fwrel_zip_entry_fn begin, fwrel_sink_fn sink,
                                 fwrel_zip_entry_fn end, void *ctx, int bundle)
{
    struct fwrel_zip *z;

    if (!sink)
        return NULL;
    z = calloc(1, sizeof(*z));
    if (!z)
        return NULL;
    z->sink = sink;
    z->begin = begin;
    z->end = end;
    z->ctx = ctx;
    z->bundle = bundle;
    z->state = ZS_LOCAL;
    z->hdr_need = ZIP_LOCAL_BYTES;
    return z;
}

struct fwrel_zip *fwrel_zip_new(fwrel_sink_fn sink, void *ctx)
{
    return zip_new(NULL, sink, NULL, ctx, 0);
}

struct fwrel_zip *fwrel_zip_new_bundle(fwrel_zip_entry_fn begin,
                                       fwrel_sink_fn sink,
                                       fwrel_zip_entry_fn end, void *ctx)
{
    return zip_new(begin, sink, end, ctx, 1);
}

void fwrel_zip_free(struct fwrel_zip *z)
{
    free(z);
}

unsigned long fwrel_zip_written(const struct fwrel_zip *z)
{
    return z->target ? z->out_total : 0;
}

static int zip_fail(struct fwrel_zip *z, int result)
{
    z->result = result;
    z->state = ZS_FINISHED;
    return result;
}

/* BOOT.bin at the root or inside exactly one directory. */
static int is_target_name(const char *name)
{
    const char *slash = strchr(name, '/');

    if (!slash)
        return strcmp(name, "BOOT.bin") == 0;
    return slash != name && strchr(slash + 1, '/') == NULL &&
           strcmp(slash + 1, "BOOT.bin") == 0;
}

static int module_name_ok(const char *name)
{
    size_t len = strlen(name);
    size_t i;

    if (len < 5 || len > 12 || strcmp(name + len - 4, ".ZZM") != 0)
        return 0;
    for (i = 0; i < len - 4; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int safe_bundle_path(const char *name)
{
    const char *part = name;

    if (!*name || *name == '/' || strchr(name, '\\'))
        return 0;
    while (*part) {
        const char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);

        if (!len || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.'))
            return 0;
        if (!slash || !slash[1])
            return 1;
        part = slash + 1;
    }
    return 0;
}

/* Return the selected entry kind, ZIP_ENTRY_NONE, or -1 for an unsafe path. */
static int bundle_target_name(struct fwrel_zip *z)
{
    const char *slash, *rest, *next;
    size_t root_len;

    if (!safe_bundle_path(z->name))
        return -1;
    slash = strchr(z->name, '/');
    if (!slash)
        return -1;
    root_len = (size_t)(slash - z->name);
    if (!z->root[0]) {
        memcpy(z->root, z->name, root_len);
        z->root[root_len] = '\0';
    } else if (strlen(z->root) != root_len ||
               memcmp(z->root, z->name, root_len) != 0) {
        return -1;
    }
    rest = slash + 1;
    if (!*rest)
        return ZIP_ENTRY_NONE;
    next = strchr(rest, '/');
    if (!next) {
        if (strcmp(rest, "BOOT.bin") != 0)
            return ZIP_ENTRY_NONE;
        if (z->found_boot)
            return -1;
        strcpy(z->entry_name, "BOOT.bin");
        return ZIP_ENTRY_BOOT;
    }
    if (strncmp(rest, "ZZ9K/", 5) != 0)
        return -1;
    rest += 5;
    if (!*rest)
        return ZIP_ENTRY_NONE; /* ZZ9K directory entry */
    if (strchr(rest, '/') || !module_name_ok(rest))
        return -1;
    snprintf(z->entry_name, sizeof(z->entry_name), "ZZ9K/%s", rest);
    return ZIP_ENTRY_MODULE;
}

static int module_header_ok(const uint8_t *head, uint32_t *image_size)
{
    size_t i;
    int terminated = 0;

    if (memcmp(head, "ZZM1", 4) != 0 || le16(head + 4) != 128 ||
        le16(head + 6) != 1)
        return 0;
    for (i = 0; i < 16; i++) {
        uint8_t c = head[36 + i];
        if (!c) {
            if (!i)
                return 0;
            terminated = 1;
        } else if (terminated ||
                   !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_')) {
            return 0;
        }
    }
    if (!terminated)
        return 0;
    for (i = 84; i < FWREL_MODULE_HEADER_BYTES; i++)
        if (head[i])
            return 0;
    *image_size = le32(head + 24);
    return *image_size <= ULONG_MAX - FWREL_MODULE_HEADER_BYTES;
}

/* Emit inflated or stored bytes of the current entry. */
static int zip_output(struct fwrel_zip *z, const uint8_t *data, size_t len)
{
    size_t image_offset = 0;

    if (!len)
        return 0;
    if (len > ULONG_MAX - z->out_total)
        return FWREL_ZIP_ERR_SIZE;
    z->crc = crc32_update(z->crc, data, len);
    if (z->target == ZIP_ENTRY_BOOT) {
        if (z->out_total < FWREL_BOOT_HEADER_BYTES) {
            size_t n = FWREL_BOOT_HEADER_BYTES - z->out_total;
            if (n > len) n = len;
            memcpy(z->head + z->out_total, data, n);
        }
        if (z->out_total + len > FWREL_BOOT_MAX_BYTES)
            return FWREL_ZIP_ERR_SIZE;
    } else if (z->target == ZIP_ENTRY_MODULE) {
        if (z->out_total < FWREL_MODULE_HEADER_BYTES) {
            size_t n = FWREL_MODULE_HEADER_BYTES - z->out_total;
            if (n > len) n = len;
            memcpy(z->head + z->out_total, data, n);
            image_offset = n;
            if (z->out_total + n == FWREL_MODULE_HEADER_BYTES) {
                if (!module_header_ok(z->head, &z->module_image_size))
                    return FWREL_ZIP_ERR_MODULE;
                z->module_header_ok = 1;
                sha256_init(&z->sha256);
            }
        }
        if (z->module_header_ok) {
            unsigned long limit = (unsigned long)FWREL_MODULE_HEADER_BYTES +
                                  z->module_image_size;
            if (z->out_total + len > limit)
                return FWREL_ZIP_ERR_SIZE;
            if (image_offset < len)
                sha256_update(&z->sha256, data + image_offset, len - image_offset);
        }
    }
    if (z->target && z->sink(z->ctx, data, len))
        return FWREL_ZIP_ERR_WRITE;
    z->out_total += len;
    return 0;
}

/* The entry's data is complete: verify it, then finish or move on. */
static int zip_entry_done(struct fwrel_zip *z)
{
    if (z->crc != z->crc_expected)
        return zip_fail(z, FWREL_ZIP_ERR_CRC);
    if (z->out_total != z->usize)
        return zip_fail(z, FWREL_ZIP_ERR_SIZE);
    if (z->target == ZIP_ENTRY_BOOT) {
        if (z->out_total < FWREL_BOOT_MIN_BYTES)
            return zip_fail(z, FWREL_ZIP_ERR_SIZE);
        if (!fwrel_boot_header_ok(z->head, FWREL_BOOT_HEADER_BYTES))
            return zip_fail(z, FWREL_ZIP_ERR_HEADER);
        z->found_boot = 1;
    } else if (z->target == ZIP_ENTRY_MODULE) {
        uint8_t digest[32];

        if (!z->module_header_ok)
            return zip_fail(z, FWREL_ZIP_ERR_MODULE);
        if (z->out_total != (unsigned long)FWREL_MODULE_HEADER_BYTES +
                            z->module_image_size)
            return zip_fail(z, FWREL_ZIP_ERR_SIZE);
        sha256_final(&z->sha256, digest);
        if (memcmp(digest, z->head + 52, sizeof(digest)) != 0)
            return zip_fail(z, FWREL_ZIP_ERR_MODULE);
    }
    if (z->target && z->end && z->end(z->ctx, z->entry_name))
        return zip_fail(z, FWREL_ZIP_ERR_WRITE);
    if (z->target && !z->bundle) {
        z->result = FWREL_ZIP_DONE;
        z->state = ZS_FINISHED;
        return FWREL_ZIP_DONE;
    }
    z->state = ZS_LOCAL;
    z->hdr_have = 0;
    z->hdr_need = ZIP_LOCAL_BYTES;
    return FWREL_ZIP_MORE;
}

/* The local header and name are in; set up the entry's data phase. */
static int zip_begin_entry(struct fwrel_zip *z)
{
    int descriptor = (z->flags & ZIP_FLAG_DESCRIPTOR) != 0;
    int target;

    target = z->bundle ? bundle_target_name(z) :
             (is_target_name(z->name) ? ZIP_ENTRY_BOOT : ZIP_ENTRY_NONE);
    if (target < 0)
        return zip_fail(z, FWREL_ZIP_ERR_FORMAT);
    z->target = target;
    z->crc = 0;
    z->out_total = 0;
    z->module_header_ok = 0;
    if (z->flags & ZIP_FLAG_ENCRYPTED)
        return zip_fail(z, FWREL_ZIP_ERR_UNSUPPORTED);
    if (!descriptor && (z->csize == 0xffffffffUL || z->usize == 0xffffffffUL))
        return zip_fail(z, FWREL_ZIP_ERR_UNSUPPORTED); /* ZIP64 */
    if (z->target && z->begin && z->begin(z->ctx, z->entry_name))
        return zip_fail(z, FWREL_ZIP_ERR_WRITE);
    if (z->method == ZIP_METHOD_DEFLATE) {
        tinfl_init(&z->inflater);
        z->dict_ofs = 0;
        z->state = ZS_DATA;
        return FWREL_ZIP_MORE;
    }
    if (z->method != ZIP_METHOD_STORE || descriptor)
        return zip_fail(z, FWREL_ZIP_ERR_UNSUPPORTED);
    if (z->csize != z->usize)
        return zip_fail(z, FWREL_ZIP_ERR_FORMAT);
    if (!z->target && z->usize == 0)
        return zip_entry_done(z);
    z->skip = z->csize;
    z->state = z->target ? ZS_DATA : ZS_SKIP;
    if (!z->target)
        z->crc = z->crc_expected; /* skipped bytes are not checked */
    return FWREL_ZIP_MORE;
}

/* Copy header bytes until hdr_need are present; returns bytes used. */
static size_t zip_collect(struct fwrel_zip *z, const uint8_t *data, size_t len)
{
    size_t n = z->hdr_need - z->hdr_have;

    if (n > len) n = len;
    memcpy(z->hdr + z->hdr_have, data, n);
    z->hdr_have += n;
    return n;
}

int fwrel_zip_feed(struct fwrel_zip *z, const uint8_t *data, size_t len)
{
    size_t i = 0;

    while (z->state != ZS_FINISHED && (i < len || z->state == ZS_DATA)) {
        switch (z->state) {
        case ZS_LOCAL:
            /* A central directory ends a complete bundle. */
            if (z->hdr_have < 4) {
                i += zip_collect(z, data + i, len - i);
                if (z->hdr_have >= 4 && le32(z->hdr) == ZIP_CENTRAL_SIG) {
                    if (z->bundle && z->found_boot) {
                        z->result = FWREL_ZIP_DONE;
                        z->state = ZS_FINISHED;
                        return FWREL_ZIP_DONE;
                    }
                    return zip_fail(z, FWREL_ZIP_ERR_NOT_FOUND);
                }
                if (z->hdr_have >= 4 && le32(z->hdr) != ZIP_LOCAL_SIG)
                    return zip_fail(z, FWREL_ZIP_ERR_FORMAT);
                break;
            }
            i += zip_collect(z, data + i, len - i);
            if (z->hdr_have < z->hdr_need)
                break;
            z->flags = le16(z->hdr + 6);
            z->method = le16(z->hdr + 8);
            z->crc_expected = le32(z->hdr + 14);
            z->csize = le32(z->hdr + 18);
            z->usize = le32(z->hdr + 22);
            z->name_len = le16(z->hdr + 26);
            z->extra_len = le16(z->hdr + 28);
            if (z->name_len == 0 || z->name_len >= sizeof(z->name))
                return zip_fail(z, FWREL_ZIP_ERR_FORMAT);
            z->hdr_have = 0;
            z->skip = (unsigned long)z->name_len + z->extra_len;
            z->state = ZS_NAME;
            break;
        case ZS_NAME: {
            size_t n = len - i;
            unsigned long consumed = (unsigned long)z->name_len + z->extra_len -
                                     z->skip;
            if (n > z->skip) n = z->skip;
            if (consumed < z->name_len) {
                size_t name_part = z->name_len - consumed;
                if (name_part > n) name_part = n;
                memcpy(z->name + consumed, data + i, name_part);
            }
            i += n;
            z->skip -= n;
            if (z->skip)
                break;
            z->name[z->name_len] = '\0';
            if (zip_begin_entry(z) < 0 || z->state == ZS_FINISHED)
                return z->result;
            break;
        }
        case ZS_SKIP: {
            size_t n = len - i;
            if (n > z->skip) n = z->skip;
            i += n;
            z->skip -= n;
            if (!z->skip) {
                z->out_total = z->usize;
                if (zip_entry_done(z) != FWREL_ZIP_MORE)
                    return z->result;
            }
            break;
        }
        case ZS_DATA:
            if (z->method == ZIP_METHOD_STORE) {
                size_t n = len - i;
                int rc;
                if (!n) return FWREL_ZIP_MORE;
                if (n > z->skip) n = z->skip;
                rc = zip_output(z, data + i, n);
                if (rc) return zip_fail(z, rc);
                i += n;
                z->skip -= n;
                if (!z->skip && zip_entry_done(z) != FWREL_ZIP_MORE)
                    return z->result;
            } else {
                /* tinfl writes into the circular 32 KiB window, which
                 * doubles as the output buffer. */
                size_t in_bytes = len - i;
                size_t out_bytes = TINFL_LZ_DICT_SIZE - z->dict_ofs;
                tinfl_status st;
                int rc;

                st = tinfl_decompress(&z->inflater, data + i, &in_bytes,
                                      z->dict, z->dict + z->dict_ofs,
                                      &out_bytes, TINFL_FLAG_HAS_MORE_INPUT);
                i += in_bytes;
                if (st < TINFL_STATUS_DONE)
                    return zip_fail(z, FWREL_ZIP_ERR_INFLATE);
                rc = zip_output(z, z->dict + z->dict_ofs, out_bytes);
                if (rc) return zip_fail(z, rc);
                z->dict_ofs = (z->dict_ofs + out_bytes) &
                              (TINFL_LZ_DICT_SIZE - 1);
                if (st == TINFL_STATUS_DONE) {
                    if (z->flags & ZIP_FLAG_DESCRIPTOR) {
                        z->hdr_have = 0;
                        z->hdr_need = 12;
                        z->state = ZS_DESCRIPTOR;
                    } else if (zip_entry_done(z) != FWREL_ZIP_MORE) {
                        return z->result;
                    }
                } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
                    if (i >= len)
                        return FWREL_ZIP_MORE;
                    if (!in_bytes && !out_bytes)
                        return zip_fail(z, FWREL_ZIP_ERR_INFLATE); /* stuck */
                }
            }
            break;
        case ZS_DESCRIPTOR:
            i += zip_collect(z, data + i, len - i);
            if (z->hdr_have < z->hdr_need)
                break;
            /* Optional signature: 16 bytes with it, 12 without. */
            if (z->hdr_need == 12 && le32(z->hdr) == ZIP_DESCRIPTOR_SIG) {
                z->hdr_need = 16;
                break;
            }
            {
                const uint8_t *d = z->hdr + (z->hdr_need == 16 ? 4 : 0);
                z->crc_expected = le32(d);
                z->usize = le32(d + 8);
            }
            if (zip_entry_done(z) != FWREL_ZIP_MORE)
                return z->result;
            break;
        }
    }
    return z->state == ZS_FINISHED ? z->result : FWREL_ZIP_MORE;
}

const char *fwrel_zip_strerror(int result)
{
    switch (result) {
    case FWREL_ZIP_DONE:            return "complete";
    case FWREL_ZIP_MORE:            return "archive ended early";
    case FWREL_ZIP_ERR_FORMAT:      return "not a valid ZIP archive";
    case FWREL_ZIP_ERR_UNSUPPORTED: return "unsupported ZIP entry";
    case FWREL_ZIP_ERR_NOT_FOUND:   return "no BOOT.bin in the archive";
    case FWREL_ZIP_ERR_INFLATE:     return "compressed data is corrupt";
    case FWREL_ZIP_ERR_CRC:         return "CRC-32 mismatch";
    case FWREL_ZIP_ERR_SIZE:        return "unexpected image size";
    case FWREL_ZIP_ERR_HEADER:      return "BOOT.bin is not a Zynq boot image";
    case FWREL_ZIP_ERR_WRITE:       return "cannot write the extracted image";
    case FWREL_ZIP_ERR_MODULE:      return "invalid firmware module";
    default:                        return "unknown error";
    }
}
