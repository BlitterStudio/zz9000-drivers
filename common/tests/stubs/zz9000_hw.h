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

#endif
