/*
 * Board register offsets for host tests. The real zz9000_hw.h pulls in
 * AmigaOS headers that do not exist off-target.
 *
 * These constants only matter to zzcfg_read_raw()/zzcfg_save(), which the
 * host tests do not exercise - they cover the pure parse/generate model.
 * They are present so the translation unit compiles, not so it can talk to
 * hardware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_TEST_ZZ9000_HW_H
#define ZZ_TEST_ZZ9000_HW_H

#define ZZ_BUFFER_OFFSET        0xA000
#define ZZ_REG_CONFIG_FILE      0xEC
#define ZZ_REG_CONFIG_FILE_LEN  0xEE
#define ZZ_FW_CAP_VIDEOCAP_PROFILE (1U << 0)
#define ZZ_FW_CAP_VIDEOCAP_LIVE    (1U << 1)
#define ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P (1U << 3)

#define ZZ_VCAP_LIVE_CAPABILITY       0x1400
#define ZZ_VCAP_LIVE_STATUS           0x1404
#define ZZ_VCAP_LIVE_APPLIED_RAW      0x1408
#define ZZ_VCAP_LIVE_EFFECTIVE_CROP   0x140c
#define ZZ_VCAP_LIVE_STAGED_RAW_HI    0x1410
#define ZZ_VCAP_LIVE_STAGED_RAW_LO    0x1412
#define ZZ_VCAP_LIVE_COMMIT           0x1414
#define ZZ_VCAP_LIVE_CAPABILITY_VALUE 0x564c010fUL
#define ZZ_VCAP_LIVE_COMMIT_TOKEN     0xca1b

#define ZZ_VCAP_STATUS_REQUEST_SHIFT  24
#define ZZ_VCAP_STATUS_APPLIED_SHIFT  16
#define ZZ_VCAP_STATUS_STANDARD_VALID (1UL << 15)
#define ZZ_VCAP_STATUS_NTSC           (1UL << 14)
#define ZZ_VCAP_STATUS_REJECTED       (1UL << 13)
#define ZZ_VCAP_STATUS_APPLIED_VALID  (1UL << 1)
#define ZZ_VCAP_STATUS_BUSY           (1UL << 0)

#define ZZ_VCAP_CROP_H_COMPAT         188UL
#define ZZ_VCAP_CROP_V_COMPAT         26UL
#define ZZ_VCAP_CROP_H_AUTO_FLAG      (1UL << 28)
#define ZZ_VCAP_CROP_V_AUTO_FLAG      (1UL << 29)

#endif
