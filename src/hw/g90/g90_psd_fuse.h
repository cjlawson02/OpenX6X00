/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Combine the G90 body's DE-9 spectrum (~48 kHz, AGC steps) with a local
 *  CS4271 IQ FFT (192 kHz, dBFS) onto one PSD row for dsp_psd().
 *
 *  Body fills bins it covers; IQ fills only the skirts. No blend in the
 *  overlap. A small EMA bias matches IQ skirt brightness to the body floor.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define G90_FUSE_BINS        512
#define G90_FUSE_SPAN_HZ     192000u
#define G90_FUSE_BODY_BINS   160
#define G90_FUSE_BODY_BIN_HZ 298
#define G90_FUSE_BODY_CENTER 80
#define G90_FUSE_FLOOR_DB    (-130.0f)
#define G90_FUSE_DB_PER_STEP 2.0f

typedef struct {
    float offset;   /* EMA: IQ dB-above-floor bias so skirts match body black */
    int   shift;    /* always 0 (kept for ABI; no lag search) */
    int   primed;
} g90_fuse_state_t;

void g90_fuse_state_reset(g90_fuse_state_t *st);

/* body: G90_FUSE_BODY_BINS AGC steps, bin G90_FUSE_BODY_CENTER on the VFO.
 * iq_db: G90_FUSE_BINS dB, lowest frequency first, span G90_FUSE_SPAN_HZ.
 * out: G90_FUSE_BINS GUI dB (floor + dB above noise). */
void g90_psd_fuse(g90_fuse_state_t *st, const uint16_t *body, const float *iq_db, float *out);

/* Put an IQ FFT row on the GUI's dB scale: the 20th percentile sits on
 * G90_FUSE_FLOOR_DB so the waterfall black is the noise, not the middle of it.
 * floor_ema is kept across frames (pass primed=0 on the first call). */
void g90_psd_iq_place(float *floor_ema, int *primed, const float *iq_db, float *out);
