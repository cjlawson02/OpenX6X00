/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Body spectrum in the DE-9 band; local IQ only outside it (no blend).
 *  See g90_psd_fuse.h.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#include "g90_psd_fuse.h"

#include <stdlib.h>
#include <string.h>

#define OFFSET_EMA 0.1f
#define NORM_MIN (-5.0f)
#define NORM_MAX 80.0f

static int cmp_float(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0.0f ? -1 : d > 0.0f ? 1 : 0;
}

static float percentile20(const float *src, const uint8_t *mask, int n, float *scratch) {
    int c = 0;

    for (int i = 0; i < n; i++) {
        if (!mask || mask[i]) {
            scratch[c++] = src[i];
        }
    }
    if (c <= 0) {
        return 0.0f;
    }
    qsort(scratch, (size_t)c, sizeof(float), cmp_float);
    return scratch[(int)(0.20f * (float)(c - 1))];
}

static float clamp_norm(float db, float floor_db) {
    float n = db - floor_db;

    if (n < NORM_MIN) {
        n = NORM_MIN;
    }
    if (n > NORM_MAX) {
        n = NORM_MAX;
    }
    return n;
}

static void map_body(const uint16_t *body, float *db, uint8_t *valid) {
    const float bin_hz = (float)G90_FUSE_SPAN_HZ / (float)G90_FUSE_BINS;

    for (int i = 0; i < G90_FUSE_BINS; i++) {
        float freq = (float)(i - G90_FUSE_BINS / 2) * bin_hz;
        float pos  = (float)G90_FUSE_BODY_CENTER + freq / (float)G90_FUSE_BODY_BIN_HZ;
        int   i0;
        float frac, step;

        if (pos < 0.0f || pos > (float)(G90_FUSE_BODY_BINS - 1)) {
            valid[i] = 0;
            db[i]    = 0.0f;
            continue;
        }
        i0   = (int)pos;
        frac = pos - (float)i0;
        if (i0 >= G90_FUSE_BODY_BINS - 1) {
            step = (float)body[G90_FUSE_BODY_BINS - 1];
        } else {
            step = (float)body[i0] * (1.0f - frac) + (float)body[i0 + 1] * frac;
        }
        db[i]    = G90_FUSE_FLOOR_DB + G90_FUSE_DB_PER_STEP * step;
        valid[i] = 1;
    }
}

void g90_psd_iq_place(float *floor_ema, int *primed, const float *iq_db, float *out) {
    float scratch[G90_FUSE_BINS];
    float floor;

    if (!floor_ema || !primed || !iq_db || !out) {
        return;
    }
    floor = percentile20(iq_db, NULL, G90_FUSE_BINS, scratch);
    if (!*primed) {
        *floor_ema = floor;
        *primed    = 1;
    } else {
        *floor_ema += OFFSET_EMA * (floor - *floor_ema);
    }
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        out[i] = G90_FUSE_FLOOR_DB + (iq_db[i] - *floor_ema);
    }
}

void g90_fuse_state_reset(g90_fuse_state_t *st) {
    if (st) {
        memset(st, 0, sizeof(*st));
    }
}

void g90_psd_fuse(g90_fuse_state_t *st, const uint16_t *body, const float *iq_db, float *out) {
    float   body_db[G90_FUSE_BINS];
    float   body_n[G90_FUSE_BINS];
    float   iq_n[G90_FUSE_BINS];
    float   scratch[G90_FUSE_BINS];
    uint8_t valid[G90_FUSE_BINS];
    uint8_t all[G90_FUSE_BINS];
    float   body_floor, iq_floor, measured;
    int     c;
    double  acc;

    if (!st || !body || !iq_db || !out) {
        return;
    }
    map_body(body, body_db, valid);
    memset(all, 1, sizeof all);
    body_floor = percentile20(body_db, valid, G90_FUSE_BINS, scratch);
    iq_floor   = percentile20(iq_db, all, G90_FUSE_BINS, scratch);
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        body_n[i] = valid[i] ? clamp_norm(body_db[i], body_floor) : 0.0f;
        iq_n[i]   = clamp_norm(iq_db[i], iq_floor);
    }

    /* Light bias so IQ skirts sit on the same black as the body band (no lag hunt). */
    acc = 0;
    c   = 0;
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        if (!valid[i]) {
            continue;
        }
        acc += (double)body_n[i] - (double)iq_n[i];
        c++;
    }
    measured = c > 0 ? (float)(acc / c) : 0.0f;
    if (!st->primed) {
        st->offset = measured;
        st->primed = 1;
    } else {
        st->offset += OFFSET_EMA * (measured - st->offset);
    }
    st->shift = 0;

    for (int i = 0; i < G90_FUSE_BINS; i++) {
        float v;

        if (valid[i]) {
            v = body_n[i];
        } else {
            v = iq_n[i] + st->offset;
        }
        out[i] = G90_FUSE_FLOOR_DB + v;
    }
}
