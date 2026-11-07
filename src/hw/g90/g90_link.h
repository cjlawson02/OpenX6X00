/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the G90's DE-9 link as hw_g90.c sees it: a C face on libg90 (C++20, MIT), one
 *  head frame out and one body frame back per exchange. The head's whole state goes every time;
 *  the body keeps nothing between frames.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G90_FFT_BINS 160

typedef struct {
    uint32_t freq_hz;
    uint8_t  mode;              /* radio_mode_t, LSB .. NFM: the same order as the wire's */
    uint16_t filter_low_hz;
    uint16_t filter_high_hz;
    uint8_t  agc;               /* radio_agc_t: the wire's numbers */
} g90_vfo_ctl_t;

typedef struct {
    g90_vfo_ctl_t vfo[2];
    bool     vfo_b;             /* B is the active VFO */
    uint8_t  volume;            /* 0 .. 28 */
    uint8_t  rf_power_w;        /* 1 .. 20 */
    bool     nb;
    uint8_t  nb_level, nb_width;    /* nibbles */
    uint8_t  sql_level;              /* 0 .. 9 */
    bool     split;
    bool     headphones;       /* route body RX AF to DE-9 phones1/2 */
    bool     tuner;
    bool     mic_comp;
    int16_t  rit_hz;            /* -512 .. 511 */
    /* TX: nothing keys unless allow_tx */
    bool     allow_tx;
    bool     ptt;
    bool     atu_tune;
    bool     shutdown;
} g90_ctl_t;

typedef struct {
    bool     crc_ok;
    int16_t  s_meter;           /* the stock unpack: byte - 256 */
    uint8_t  po, swr, alc;      /* raw meters */
    uint8_t  voltage_raw;       /* 0.1 V */
    uint8_t  atu;               /* byte 4 bits 0-1 */
    uint8_t  ver_major, ver_minor;  /* bytes 7 and 6, hex digits: 0x01, 0x81 is 1.81 */
    uint16_t fft[G90_FFT_BINS]; /* bin 80 on the active VFO, ~298 Hz a bin */
} g90_body_t;

typedef struct g90_link g90_link_t;

/* profile: "1.78", "1.79", "1.81"; NULL for 1.81. NULL on failure, the reason in err */
g90_link_t *g90_link_open(const char *path, const char *profile, char *err, size_t err_len);
void        g90_link_close(g90_link_t *link);
bool        g90_link_exchange(g90_link_t *link, const g90_ctl_t *ctl, g90_body_t *body, int timeout_ms,
                              char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
