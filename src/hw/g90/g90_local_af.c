/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#define _GNU_SOURCE
#include "g90_local_af.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AF_SRC "alsa_input.platform-1c22c00.codec.stereo-fallback"
#define AF_SNK "alsa_output.platform-1c22c00.codec.stereo-fallback"

static int  module_id = -1;
static int  last_pct = -1;
static int  analog = -1;        /* G90_AF_ANALOG=1: the codec's own Mic1 -> mixer -> amplifier path; -1 not read yet */
static bool analog_on;

/* The analog path (opt-in, G90_AF_ANALOG=1): Mic1 to the output mixers in the codec itself, no capture/playback
 * through the CPU (the Pulse loopback was 8-17 % of a core at ~290 wakeups a second). VOL as the loopback's gain
 * (100 % unity): "Mic" (mixer, -4.5..+6 dB in 1.5 dB steps) up to the gain, "Power Amplifier" (-62..0 dB, 1 dB
 * steps, its 0 a mute) the rest down. Tops out at +6 dB: Mic1 Boost (+24 dB and up) stays 0, the input is line
 * level. Capture of Mic1 for the decoders is not changed (DAPM, Pulse's source). Not yet heard on a radio:
 * docs/subsystems/performance.md */
static void analog_set(int pct) {
    char cmd[256];

    if (pct <= 0) {
        snprintf(cmd, sizeof(cmd), "amixer -c 0 -q sset 'Power Amplifier' 0; amixer -c 0 -q sset 'Mic1' mute");
    } else {
        float db = 60.0f * log10f(pct / 100.0f);   /* Pulse's volume scale is cubic: the loopback's dB at this VOL */
        float mic, pa;

        if (db > 6.0f) {
            db = 6.0f;
        }
        if (db < -62.0f) {
            db = -62.0f;
        }
        mic = ceilf(db / 1.5f) * 1.5f;      /* the step at or above, the amplifier down from it */
        if (mic < -4.5f) {
            mic = -4.5f;
        }
        if (mic > 6.0f) {
            mic = 6.0f;
        }
        pa = roundf(db - mic);
        if (pa > 0.0f) {
            pa = 0.0f;
        }
        /* Raw steps, not "dB": amixer set -0.0dB as +3 dB. Mic 0..7 is -4.5..+6 dB, the amplifier 1..63 is
         * -62..0 dB */
        snprintf(cmd, sizeof(cmd), "amixer -c 0 -q sset 'Mic' %d; amixer -c 0 -q sset 'Power Amplifier' %d; "
                 "amixer -c 0 -q sset 'Mic1' unmute", (int) lroundf((mic + 4.5f) / 1.5f), 63 + (int) pa);
    }
    if (system(cmd)) {
        /* best effort */
    }
}

static void analog_start(void) {
    /* A loopback left by a GUI before this one would play over the analog path: dropped */
    if (system("pactl list short modules | awk '$2==\"module-loopback\" && index($0, \"" AF_SRC "\"){print $1}' "
               "| xargs -r -n1 pactl unload-module >/dev/null 2>&1")) {
        /* best effort */
    }
    /* Muted first: the level comes with the caller's gain */
    if (system("amixer -c 0 -q sset 'Power Amplifier' 0; amixer -c 0 -q sset 'Mic1' mute; "
               "amixer -c 0 -q sset 'Mic1 Boost' 0; amixer -c 0 -q sset 'Power Amplifier DAC' unmute; "
               "amixer -c 0 -q sset 'Power Amplifier Mixer' unmute; "
               "amixer -c 0 -q sset 'Power Amplifier Mute' unmute >/dev/null 2>&1")) {
        /* best effort */
    }
    analog_on = true;
    last_pct = -1;
}

static void analog_stop(void) {
    if (system("amixer -c 0 -q sset 'Mic1' mute; amixer -c 0 -q sset 'Power Amplifier Mixer' mute; "
               "amixer -c 0 -q sset 'Power Amplifier' 100% >/dev/null 2>&1")) {
        /* best effort */
    }
    analog_on = false;
    last_pct = -1;
}

static int load_loopback(void) {
    FILE *p = popen("pactl load-module module-loopback source=" AF_SRC " sink=" AF_SNK
                    " latency_msec=80 2>/dev/null", "r");
    int   id = -1;

    if (p) {
        if (fscanf(p, "%d", &id) != 1) {
            id = -1;
        }
        pclose(p);
    }
    return id;
}

/* `pactl list short sink-inputs` has no owner-module column: the loopback is the one whose driver is
 * module-loopback.c. It can take a moment to appear after the load. Synchronous, so quick VOL steps
 * land in order; the caller is a settings call, not the render path. */
static void set_gain(int pct) {
    char cmd[128];

    for (int i = 0; i < 10; i++) {
        FILE *p = popen("pactl list short sink-inputs | awk '$4==\"module-loopback.c\"{print $1}'", "r");
        int   idx = -1;

        if (p) {
            if (fscanf(p, "%d", &idx) != 1) {
                idx = -1;
            }
            pclose(p);
        }
        if (idx >= 0) {
            snprintf(cmd, sizeof(cmd), "pactl set-sink-input-volume %d %d%% >/dev/null 2>&1", idx, pct);
            if (system(cmd)) {
                /* best effort */
            }
            return;
        }
        usleep(200000);
    }
}

void g90_local_af_set(bool on, int gain_pct) {
    if (gain_pct < 0) {
        gain_pct = 0;
    }
    if (!on) {
        g90_local_af_stop();
        return;
    }
    if (analog < 0) {
        const char *e = getenv("G90_AF_ANALOG");

        analog = e && !strcmp(e, "1");
    }
    if (analog) {
        if (!analog_on) {
            analog_start();
        }
        if (gain_pct != last_pct) {
            analog_set(gain_pct);
            last_pct = gain_pct;
        }
        return;
    }
    if (module_id < 0) {
        /* Mic1 is the DE-9 phones net; its hardware gain stays at 0, the level is the loopback's. The
         * speaker amp is unmuted but the loopback starts at the caller's gain, 0 when VOL is 0. */
        if (system("amixer -c 0 -q sset 'Capture Source' 'Mic1 Mono'; amixer -c 0 -q sset 'Mic1 Boost' 0; "
                   "amixer -c 0 -q sset 'Capture Boost' 0; amixer -c 0 -q sset 'Power Amplifier' 100%; "
                   "amixer -c 0 -q sset 'Power Amplifier DAC' unmute; amixer -c 0 -q sset 'Power Amplifier Mute' unmute; "
                   "amixer -c 0 -q sset 'Power Amplifier Mixer' mute >/dev/null 2>&1")) {
            /* best effort */
        }
        /* A crash leaves its loopback behind: drop ours before loading another */
        if (system("pactl list short modules | awk '$2==\"module-loopback\" && index($0, \"" AF_SRC "\"){print $1}' "
                   "| xargs -r -n1 pactl unload-module >/dev/null 2>&1")) {
            /* best effort */
        }
        module_id = load_loopback();
        last_pct = -1;
        if (module_id < 0) {
            fprintf(stderr, "hw_g90: local AF loopback did not load\n");
            return;
        }
    }
    if (gain_pct != last_pct) {
        set_gain(gain_pct);
        last_pct = gain_pct;
    }
}

void g90_local_af_stop(void) {
    char cmd[64];

    if (analog_on) {
        analog_stop();
    }
    if (module_id >= 0) {
        snprintf(cmd, sizeof(cmd), "pactl unload-module %d >/dev/null 2>&1", module_id);
        if (system(cmd)) {
            /* best effort */
        }
        module_id = -1;
        last_pct = -1;
    }
}
