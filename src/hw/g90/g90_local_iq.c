/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Local IQ for the fused waterfall. One 512-point Hann FFT of hw:1,0, published
 *  a few dozen times a second. The fuse thread only copies the latest row.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#define _GNU_SOURCE
#include "g90_local_iq.h"

#include <alsa/asoundlib.h>
#include <complex.h>
#include <liquid/liquid.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define IQ_RATE   G90_FUSE_SPAN_HZ
#define IQ_CH     2
#define FFT_EVERY 8     /* consume every block; transform every 8th (~40 Hz at 512/192k) */

static pthread_t        thr;
static pthread_mutex_t  mu = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t       *pcm;
static atomic_bool      stop;
static int              started;
static int              ready;
static int              ready_logged;
static int              swap_iq;
static float            latest[G90_FUSE_BINS];
static float            win[G90_FUSE_BINS];

static float s24_to_float(int32_t w) {
    /* S24_LE sits in the low 24 bits of a 32-bit slot. Sign-extend without a signed overflow. */
    uint32_t u = ((uint32_t)w << 8) >> 8;

    if (u & 0x800000u) {
        u |= 0xFF000000u;
    }
    return (float)(int32_t)u * (1.0f / 8388608.0f);
}

static void publish_fft(const int32_t *interleaved, liquid_float_complex *time, liquid_float_complex *freq,
                        fftplan plan, float *db) {
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        float ii = s24_to_float(interleaved[i * 2]);
        float qq = s24_to_float(interleaved[i * 2 + 1]);
        float re = swap_iq ? qq : ii;
        float im = swap_iq ? ii : qq;

        time[i] = (re + I * im) * win[i];
    }
    fft_execute(plan);
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        int            k  = (i + G90_FUSE_BINS / 2) % G90_FUSE_BINS; /* DC in the middle */
        float          re = crealf(freq[k]);
        float          im = cimagf(freq[k]);
        float          p  = re * re + im * im;

        db[i] = 10.0f * log10f(p + 1e-20f);
    }
    pthread_mutex_lock(&mu);
    memcpy(latest, db, sizeof latest);
    ready = 1;
    pthread_mutex_unlock(&mu);
    if (!ready_logged) {
        float lo = db[0], hi = db[0];

        ready_logged = 1;
        for (int i = 1; i < G90_FUSE_BINS; i++) {
            if (db[i] < lo) lo = db[i];
            if (db[i] > hi) hi = db[i];
        }
        fprintf(stderr, "hw_g90 iq: first FFT %.1f..%.1f dB\n", lo, hi);
    }
}

static void *capture_thread(void *arg) {
    int32_t               *buf  = NULL;
    liquid_float_complex *time = NULL;
    liquid_float_complex *freq = NULL;
    float                 *db   = NULL;
    fftplan                plan = NULL;
    int                    filled = 0;
    unsigned               skip   = 0;

    (void)arg;
    buf  = calloc(G90_FUSE_BINS * IQ_CH, sizeof(int32_t));
    time = calloc(G90_FUSE_BINS, sizeof(liquid_float_complex));
    freq = calloc(G90_FUSE_BINS, sizeof(liquid_float_complex));
    db   = calloc(G90_FUSE_BINS, sizeof(float));
    if (!buf || !time || !freq || !db) {
        fprintf(stderr, "hw_g90 iq: out of memory\n");
        goto done;
    }
    plan = fft_create_plan(G90_FUSE_BINS, time, freq, LIQUID_FFT_FORWARD, 0);
    if (!plan) {
        fprintf(stderr, "hw_g90 iq: fft plan failed\n");
        goto done;
    }
    /* snd_pcm_wait on a capture that is only PREPARED times out and never starts
     * the DMA. The first read starts it. */
    if (snd_pcm_start(pcm) < 0) {
        fprintf(stderr, "hw_g90 iq: start failed\n");
    }
    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        snd_pcm_sframes_t n;
        snd_pcm_uframes_t want;

        if (snd_pcm_wait(pcm, 200) == 0) {
            if (snd_pcm_state(pcm) != SND_PCM_STATE_RUNNING) {
                snd_pcm_start(pcm);
            }
            continue;
        }
        if (atomic_load_explicit(&stop, memory_order_acquire)) {
            break;
        }
        want = (snd_pcm_uframes_t)(G90_FUSE_BINS - filled);
        n    = snd_pcm_readi(pcm, buf + filled * IQ_CH, want);
        if (n == -EPIPE) {
            snd_pcm_prepare(pcm);
            filled = 0;
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "hw_g90 iq: read: %s\n", snd_strerror((int)n));
            if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
                break;
            }
            continue;
        }
        filled += (int)n;
        if (filled < G90_FUSE_BINS) {
            continue;
        }
        filled = 0;
        if (++skip < FFT_EVERY) {
            continue;
        }
        skip = 0;
        publish_fft(buf, time, freq, plan, db);
    }
done:
    if (plan) {
        fft_destroy_plan(plan);
    }
    free(buf);
    free(time);
    free(freq);
    free(db);
    return NULL;
}

/* Stock IFE: PH13 (gpio 237) low selects the IQ jack. Left as an input, the
 * codec sits on a flat −51 dBFS floor. G90_IFE=1 drives it high. */
static void ife_gpio(void) {
    const char *ife = getenv("G90_IFE");
    const char *val = ife && !strcmp(ife, "1") ? "1" : "0";
    FILE       *f;

    f = fopen("/sys/class/gpio/export", "w");
    if (f) {
        fputs("237\n", f);
        fclose(f);
    }
    f = fopen("/sys/class/gpio/gpio237/direction", "w");
    if (f) {
        fputs("out\n", f);
        fclose(f);
    }
    f = fopen("/sys/class/gpio/gpio237/value", "w");
    if (!f) {
        fprintf(stderr, "hw_g90 iq: PH13 not set\n");
        return;
    }
    fprintf(f, "%s\n", val);
    fclose(f);
    fprintf(stderr, "hw_g90 iq: PH13=%s\n", val);
}

bool g90_local_iq_start(void) {
    const char       *dev = getenv("G90_IQ_DEV");
    const char       *sw  = getenv("G90_IQ_SWAP");
    snd_pcm_hw_params_t *hp = NULL;
    unsigned          rate = IQ_RATE;
    snd_pcm_uframes_t period = G90_FUSE_BINS;
    int               err;

    if (started) {
        return pcm != NULL;
    }
    dev     = dev && *dev ? dev : "hw:1,0";
    swap_iq = sw && !strcmp(sw, "1");
    ife_gpio();
    for (int i = 0; i < G90_FUSE_BINS; i++) {
        win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(G90_FUSE_BINS - 1)));
    }
    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "hw_g90 iq: open %s: %s\n", dev, snd_strerror(err));
        pcm = NULL;
        return false;
    }
    snd_pcm_hw_params_malloc(&hp);
    snd_pcm_hw_params_any(pcm, hp);
    snd_pcm_hw_params_set_access(pcm, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hp, SND_PCM_FORMAT_S24_LE);
    snd_pcm_hw_params_set_channels(pcm, hp, IQ_CH);
    snd_pcm_hw_params_set_rate_near(pcm, hp, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(pcm, hp, &period, 0);
    err = snd_pcm_hw_params(pcm, hp);
    snd_pcm_hw_params_free(hp);
    if (err < 0 || rate != IQ_RATE) {
        fprintf(stderr, "hw_g90 iq: hw params (%s), rate %u want %u\n",
                err < 0 ? snd_strerror(err) : "ok", rate, IQ_RATE);
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }
    atomic_store_explicit(&stop, false, memory_order_release);
    ready   = 0;
    started = 1;
    if (pthread_create(&thr, NULL, capture_thread, NULL) != 0) {
        fprintf(stderr, "hw_g90 iq: thread failed\n");
        snd_pcm_close(pcm);
        pcm     = NULL;
        started = 0;
        return false;
    }
    fprintf(stderr, "hw_g90 iq: %s S24 %u Hz%s\n", dev, rate, swap_iq ? " swapped" : "");
    return true;
}

void g90_local_iq_stop(void) {
    if (!started) {
        return;
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    if (pcm) {
        snd_pcm_drop(pcm);
    }
    pthread_join(thr, NULL);
    if (pcm) {
        snd_pcm_close(pcm);
        pcm = NULL;
    }
    started = 0;
    ready   = 0;
}

bool g90_local_iq_copy(float *db) {
    int ok;

    if (!db) {
        return false;
    }
    pthread_mutex_lock(&mu);
    ok = ready;
    if (ok) {
        memcpy(db, latest, sizeof latest);
    }
    pthread_mutex_unlock(&mu);
    return ok;
}
