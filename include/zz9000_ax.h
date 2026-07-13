/*
 * Shared ZZ9000AX audio constants.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9000_AX_H
#define ZZ9000_AX_H

#include <stdint.h>

#include "zz9000_hw.h"

#define ZZ_AX_DEVF_INT2MODE 1

#define ZZ_AX_BYTES_PER_PERIOD 3840
#define ZZ_AX_AUDIO_PERIODS    8
#define ZZ_AX_AUDIO_RX_RESIDENT_PERIODS (ZZ_AX_AUDIO_PERIODS - 1)
#define ZZ_AX_AUDIO_BUFSZ      (ZZ_AX_BYTES_PER_PERIOD * ZZ_AX_AUDIO_PERIODS)
#define ZZ_AX_BOUNCE_BUFSZ     ZZ_AX_BYTES_PER_PERIOD
#define ZZ_AX_BOUNCE_MAX_FRAMES (ZZ_AX_BOUNCE_BUFSZ / 4)
#define ZZ_AX_RX_BUFFER_DELTA   0x8000

#define ZZ_AX_AUDIO_CONFIG_PLAY   0x0001
#define ZZ_AX_AUDIO_CONFIG_RECORD 0x0002
#define ZZ_AX_AUDIO_CONFIG_MASK   0x0003

#define ZZ_AX_AUDIO_RX_STATUS_CAPABLE       0x8000
#define ZZ_AX_AUDIO_RX_STATUS_PERIOD_SHIFT  12
#define ZZ_AX_AUDIO_RX_STATUS_PERIOD_MASK   0x7000
#define ZZ_AX_AUDIO_RX_STATUS_SEQUENCE_MASK 0x0fff

static inline uint8_t zz_ax_audio_rx_status_period(uint16_t status)
{
    return (uint8_t)((status & ZZ_AX_AUDIO_RX_STATUS_PERIOD_MASK) >>
                     ZZ_AX_AUDIO_RX_STATUS_PERIOD_SHIFT);
}

static inline uint16_t zz_ax_audio_rx_status_sequence(uint16_t status)
{
    return status & ZZ_AX_AUDIO_RX_STATUS_SEQUENCE_MASK;
}

static inline uint16_t zz_ax_audio_rx_sequence_distance(uint16_t newer,
                                                         uint16_t older)
{
    return (newer - older) & ZZ_AX_AUDIO_RX_STATUS_SEQUENCE_MASK;
}

#define ZZ_AX_DECODER_FIFO_SIZE (1152 * 4)
#define ZZ_AX_DECODE_CLEAR      0
#define ZZ_AX_DECODE_INIT       1
#define ZZ_AX_DECODE_RUN        2

#define ZZ_AX_DEFAULT_MIX_LEVELS 0x8080
#define ZZ_AX_MIX_LEVELS_ENV    "ENV:ZZ9K_MIX_LEVELS"
#define ZZ_AX_INT2_ENV          "ENV:ZZ9K_INT2"
#define ZZ_AX_NOLPF_ENV         "ENV:ZZ9000AX-NOLPF"

#define ZZ_AX_IRQ_NAME_AHI      "ZZ9000AX"
#define ZZ_AX_IRQ_NAME_MHI      "mhizz9000"
#define ZZ_AX_IRQ_NAME_MHI_SOFT "mhizz9000s"

#define ZZ_AX_AP_TX_BUF_OFFS_HI       0
#define ZZ_AX_AP_TX_BUF_OFFS_LO       1
#define ZZ_AX_AP_RX_BUF_OFFS_HI       2
#define ZZ_AX_AP_RX_BUF_OFFS_LO       3
#define ZZ_AX_AP_DSP_PROG_OFFS_HI     4
#define ZZ_AX_AP_DSP_PROG_OFFS_LO     5
#define ZZ_AX_AP_DSP_PARAM_OFFS_HI    6
#define ZZ_AX_AP_DSP_PARAM_OFFS_LO    7
#define ZZ_AX_AP_DSP_UPLOAD           8
#define ZZ_AX_AP_DSP_SET_LOWPASS      9
#define ZZ_AX_AP_DSP_SET_VOLUMES      10
#define ZZ_AX_AP_DSP_SET_PREFACTOR    11
#define ZZ_AX_AP_DSP_SET_EQ_BAND1     12
#define ZZ_AX_AP_DSP_SET_EQ_BAND2     13
#define ZZ_AX_AP_DSP_SET_EQ_BAND3     14
#define ZZ_AX_AP_DSP_SET_EQ_BAND4     15
#define ZZ_AX_AP_DSP_SET_EQ_BAND5     16
#define ZZ_AX_AP_DSP_SET_EQ_BAND6     17
#define ZZ_AX_AP_DSP_SET_EQ_BAND7     18
#define ZZ_AX_AP_DSP_SET_EQ_BAND8     19
#define ZZ_AX_AP_DSP_SET_EQ_BAND9     20
#define ZZ_AX_AP_DSP_SET_EQ_BAND10    21
#define ZZ_AX_AP_DSP_SET_STEREO_VOLUME 22
#define ZZ_AX_NUM_AUDIO_PARAMS        23

#endif /* ZZ9000_AX_H */
