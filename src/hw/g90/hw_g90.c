/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a Xiegu G90's body behind hw.h, over the DE-9 head link its own head (or a GSOC)
 *  uses, with the G90's stock firmware: libg90 through g90_link.h. Each hw_flow_read() is one
 *  ping-pong: the whole head state out, one body frame back (~25 a second). The body sends its own
 *  160-bin spectrum, about 48 kHz around the active VFO (HW_CAP_PSD, no zoom), and raw meters.
 *
 *  Compile-time defaults are BOARD_G90_DEV, BOARD_G90_PROFILE, and BOARD_G90_ALLOW_TX in board.h.
 *  G90_DEV, G90_PROFILE, and G90_ALLOW_TX override them when set and non-empty; G90_ALLOW_TX must
 *  be 1 to allow PTT and ATU tune on the wire, otherwise they are dropped.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#define _GNU_SOURCE
#include "../hw.h"
#include "../../board.h"
#include "g90_link.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXCHANGE_MS     250     /* write + body hunt with CRC slip, past the ~40 ms wire floor */
#define RETRY_US        40000   /* radio.c spins on a false hw_flow_read(): wait here instead */
#define BIN_HZ          298
/* dsp_psd() stretches a spectrum narrower than the screen (100 kHz at zoom 1) over all of it: the
 * body's 160 bins (~48 kHz) go in the middle of a wider row, the rest at a floor below the noise */
#define PSD_BINS        336
#define PSD_SPAN_HZ     (PSD_BINS * BIN_HZ)
#define PSD_PAD_DB      -160.0f
/* The body's bins are AGC-relative levels (0 .. ~38; the stock head draws min(bin x 10, 383)), not
 * dBm: an uncalibrated straight line onto the GUI's dB scale */
#define PSD_FLOOR_DB    -130.0f
#define PSD_DB_PER_STEP 2.0f
/* The stock GSOC draws the S-meter as (s + 145) / 1.45 %; uncalibrated against dBm */
#define SMETER_DBM_OFFS 18
#define VOL_MAX_GUI     BOARD_VOL_MAX
#define VOL_MAX_WIRE    28

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static g90_link_t     *link_;
static g90_ctl_t       ctl;     /* under mu: radio.c's thread writes, the flow thread reads */
static int32_t         reg_i[HW_PARAM_COUNT];
static g90_body_t      body;
static float           psd_db[PSD_BINS];
static radio_base_ver_t ver;
static char            ver_str[16] = "?";
static bool            allow_tx;

/* ---- Init ----------------------------------------------------------------------------- */

bool hw_init(void) {
    const char *dev  = getenv("G90_DEV");
    const char *prof = getenv("G90_PROFILE");
    const char *tx   = getenv("G90_ALLOW_TX");
    char        err[128] = "";

    dev  = dev && *dev ? dev : BOARD_G90_DEV;
    prof = prof && *prof ? prof : BOARD_G90_PROFILE;
    if (tx && *tx) {
        allow_tx = !strcmp(tx, "1");
    } else {
        allow_tx = BOARD_G90_ALLOW_TX;
    }
    link_    = g90_link_open(dev, prof, err, sizeof(err));
    if (!link_) {
        fprintf(stderr, "hw_g90: %s\n", err);
        return false;
    }
    ctl.vfo[0]     = (g90_vfo_ctl_t){14074000, radio_mode_usb_dig, 200, 2700, radio_agc_auto};
    ctl.vfo[1]     = (g90_vfo_ctl_t){7074000, radio_mode_usb_dig, 200, 2700, radio_agc_auto};
    ctl.volume     = 8;
    ctl.rf_power_w = 5;
    ctl.allow_tx   = allow_tx;
    fprintf(stderr, "hw_g90: %s, profile %s, TX %s\n", dev, prof, allow_tx ? "allowed" : "blocked");
    return true;
}

static bool exchange(void) {
    g90_ctl_t c;
    char      err[128] = "";

    pthread_mutex_lock(&mu);
    c = ctl;
    pthread_mutex_unlock(&mu);
    if (!g90_link_exchange(link_, &c, &body, EXCHANGE_MS, err, sizeof(err))) {
        static unsigned fails;
        if (fails++ % 25 == 0) {
            fprintf(stderr, "hw_g90: %s (%u)\n", err, fails);
        }
        return false;
    }
    return true;
}

bool hw_control_init(void) {
    return exchange();
}

/* No reset line to the body over the DE-9 */
void hw_bb_reset(void) {
}

/* The body's version: bytes 7 and 6 as hex digits, the stock head's "%-2X.%2X" */
void hw_start(void) {
    unsigned a = 0, b = 0;

    snprintf(ver_str, sizeof(ver_str), "%X.%02X", body.ver_major, body.ver_minor);
    if (sscanf(ver_str, "%u.%u", &a, &b) == 2) {
        ver = (radio_base_ver_t){(uint8_t)a, (uint8_t)b, 0, 0};
    }
}

radio_base_ver_t hw_base_ver(void) {
    return ver;
}

const char *hw_base_ver_str(void) {
    return ver_str;
}

/* The spectrum ready-made at one width. Where CW's spectrum and tone sit on the G90 is not known yet:
 * the X6100's way (no CW caps) until a capture says otherwise */
uint32_t hw_caps(void) {
    return HW_CAP_PSD;
}

/* ---- Settings ------------------------------------------------------------------------- */

static bool has_register(hw_param_t p) {
    switch (p) {
        case HW_RIT: case HW_XIT: case HW_AGC_TIME: case HW_TX_I_OFFSET: case HW_TX_Q_OFFSET:
        case HW_FILTER1_LOW: case HW_FILTER1_HIGH: case HW_FILTER2_LOW: case HW_FILTER2_HIGH:
        case HW_ATU_NETWORK:
            return true;
        default:
            return false;
    }
}

int32_t hw_get_i(hw_param_t p) {
    return (p < HW_PARAM_COUNT && has_register(p)) ? reg_i[p] : 0;
}

/* The GUI keeps one filter for the current mode; the wire has one per VFO: both follow it, as the
 * stock head does. AM and FM send low as -high: the wire takes edges from 0 up */
static uint16_t edge(int32_t hz) {
    if (hz < 0) hz = 0;
    return hz > 0xFFFF ? 0xFFFF : (uint16_t)hz;
}

bool hw_set_i(hw_param_t p, int32_t v) {
    if (p >= HW_PARAM_COUNT) {
        return false;
    }
    if (has_register(p) && reg_i[p] == v) {
        return false;
    }
    reg_i[p] = v;

    pthread_mutex_lock(&mu);
    switch (p) {
        case HW_VFO_CUR:        ctl.vfo_b = v == RADIO_VFO_B; break;
        case HW_SPLIT:          ctl.split = v; break;
        case HW_RXVOL:          ctl.volume = (uint8_t)((v < 0 ? 0 : v > VOL_MAX_GUI ? VOL_MAX_GUI : v) * VOL_MAX_WIRE / VOL_MAX_GUI); break;
        case HW_ATU_ENABLED:    ctl.tuner = v; break;
        case HW_ATU_TUNE:       ctl.atu_tune = allow_tx && v; break;
        case HW_PTT:            ctl.ptt = allow_tx && v; break;
        case HW_RIT:            ctl.rit_hz = (int16_t)v; break;
        case HW_NB:             ctl.nb = v; break;
        case HW_NB_LEVEL:       ctl.nb_level = (uint8_t)v; break;
        case HW_NB_WIDTH:       ctl.nb_width = (uint8_t)v; break;
        case HW_SQL:            ctl.sql_level = (uint8_t)(v < 0 ? 0 : v > BOARD_SQL_MAX ? BOARD_SQL_MAX : v); break;
        /* radio.c's compressor: COMP_ON true is OFF (the X6100's sense, see hw_x6200.c) */
        case HW_COMP_ON:        ctl.mic_comp = !v; break;
        case HW_FILTER1_LOW:    ctl.vfo[0].filter_low_hz = ctl.vfo[1].filter_low_hz = edge(v); break;
        case HW_FILTER1_HIGH:   ctl.vfo[0].filter_high_hz = ctl.vfo[1].filter_high_hz = edge(v); break;
        default:
            /* Not on the DE-9 wire, or not mapped yet: kept for hw_get_i() only */
            break;
    }
    pthread_mutex_unlock(&mu);
    if ((p == HW_PTT || p == HW_ATU_TUNE) && v && !allow_tx) {
        fprintf(stderr, "hw_g90: TX blocked (G90_ALLOW_TX is not 1)\n");
    }
    return true;
}

bool hw_set_f(hw_param_t p, float v) {
    if (p == HW_TXPWR_F) {
        int w = (int)(v + 0.5f);
        pthread_mutex_lock(&mu);
        ctl.rf_power_w = (uint8_t)(w < 1 ? 1 : w > 20 ? 20 : w);
        pthread_mutex_unlock(&mu);
        return true;
    }
    return false;
}

bool hw_vfo_set_freq(radio_vfo_t v, int32_t hz) {
    bool changed;

    pthread_mutex_lock(&mu);
    changed = ctl.vfo[v & 1].freq_hz != (uint32_t)hz;
    ctl.vfo[v & 1].freq_hz = (uint32_t)hz;
    pthread_mutex_unlock(&mu);
    return changed;
}

bool hw_vfo_set_mode(radio_vfo_t v, radio_mode_t mode) {
    bool changed;

    pthread_mutex_lock(&mu);
    changed = ctl.vfo[v & 1].mode != (uint8_t)mode;
    ctl.vfo[v & 1].mode = (uint8_t)mode;
    pthread_mutex_unlock(&mu);
    return changed;
}

void hw_vfo_set_agc(radio_vfo_t v, radio_agc_t agc) {
    pthread_mutex_lock(&mu);
    ctl.vfo[v & 1].agc = (uint8_t)agc;
    pthread_mutex_unlock(&mu);
}

/* libg90 sends the VFO block's ATT byte as 0; PRE is a CI-V setting on the G90 */
void hw_vfo_set_att(radio_vfo_t v, bool on) {
    (void)v; (void)on;
}

void hw_vfo_set_pre(radio_vfo_t v, bool on) {
    (void)v; (void)on;
}

/* Assert shutdown_req for ~1.5 s (several ping-pongs) so the MainUnit powers
 * down with the head — drops DE-9 pin 1 (+8 V). Same pulse gsoc-panel PWR used.
 * Armbian halt also runs tools/gsoc-dt/sd-overlay/g90-body-shutdown.service. */
void hw_poweroff(void) {
    pthread_mutex_lock(&mu);
    ctl.shutdown = true;
    ctl.ptt = false;
    ctl.atu_tune = false;
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 6 && link_; i++) {
        if (!exchange()) {
            usleep(RETRY_US);
        }
        usleep(200000);
    }
}

/* Every exchange carries the whole state already */
bool hw_resend_all(void) {
    return true;
}

bool hw_tx_keyed_by_gui(void) {
    bool keyed;

    pthread_mutex_lock(&mu);
    keyed = ctl.ptt || ctl.atu_tune;
    pthread_mutex_unlock(&mu);
    return keyed;
}

/* The GSOC's backlight and the rest are board pins, not the body's */
void hw_pin_set(hw_pin_t pin, bool on) {
    (void)pin; (void)on;
}

/* ---- The packet stream -------------------------------------------------------------------- */

bool hw_flow_read(hw_pack_t *p) {
    if (!link_ || !exchange()) {
        usleep(RETRY_US);
        return false;
    }
    const size_t off = (PSD_BINS - G90_FFT_BINS) / 2;

    for (size_t i = 0; i < PSD_BINS; i++) {
        psd_db[i] = i >= off && i < off + G90_FFT_BINS ? PSD_FLOOR_DB + PSD_DB_PER_STEP * (float)body.fft[i - off]
                                                       : PSD_PAD_DB;
    }
    memset(p, 0, sizeof(*p));
    p->psd        = psd_db;
    p->n_psd      = PSD_BINS;
    p->psd_span   = PSD_SPAN_HZ;
    p->smeter_dbm = (int16_t)(body.s_meter + SMETER_DBM_OFFS);
    p->tx         = body.po > 0;
    /* Raw meters; their scales on the 1.81 body are not pinned (remote-xcomm.md) */
    p->tx_power   = body.po;
    p->vswr       = body.swr < 10 ? 10 : body.swr;
    p->alc_level  = body.alc;
    p->vext       = body.voltage_raw;
    p->vext_flag  = true;
    p->atu_status = body.atu == 1;
    return true;
}

void hw_flow_restart(void) {
}
