/*
 * Configuration for the inflate-only subset of miniz 3.1.2
 * (https://github.com/richgel999/miniz, MIT, see LICENSE). miniz_tinfl.c,
 * miniz_tinfl.h and miniz_common.h are unmodified upstream files; this
 * header replaces the amalgamated miniz.h they include.
 *
 * The CI toolchain's prebuilt libz.a inflates the firmware release archive
 * incorrectly on m68k (CRC-32 mismatch on the real v2.8.1 BOOT.bin), so
 * ZZTop carries this small, endian-neutral inflater, built without the
 * Bebbo "bbb" peephole pass that miscompiles it (ZZTop/build-gcc.sh).
 */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
/* Byte loads only: 68000-safe and independent of host byte order. */
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_HAS_64BIT_REGISTERS 0
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define MINIZ_LITTLE_ENDIAN 1
#else
#define MINIZ_LITTLE_ENDIAN 0
#endif

#include "miniz_common.h"
#include "miniz_tinfl.h"
