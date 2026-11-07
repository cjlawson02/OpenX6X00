/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#define _GNU_SOURCE
#include "g90_local_af.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AF_SRC "alsa_input.platform-1c22c00.codec.stereo-fallback"
#define AF_SNK "alsa_output.platform-1c22c00.codec.stereo-fallback"

static int  module_id = -1;
static int  last_pct = -1;

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

    if (module_id >= 0) {
        snprintf(cmd, sizeof(cmd), "pactl unload-module %d >/dev/null 2>&1", module_id);
        if (system(cmd)) {
            /* best effort */
        }
        module_id = -1;
        last_pct = -1;
    }
}
