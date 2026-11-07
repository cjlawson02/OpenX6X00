/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GSOC local RX audio: with head ctrl1 bit 0x04 set the body mutes its speaker and puts the
 *  demodulated AF on DE-9 phones1/2, which arrives on the sun4i codec's Mic1 ("SpareMic"), not Line In
 *  (ears-on 2026-11-07, NEEDS-HARDWARE G16). Plays it on the cabinet speaker through a Pulse loopback.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include <stdbool.h>

/* on: route Mic1 to the cabinet speaker. gain_pct: Pulse gain on the loopback, 0 = silent, 100 = unity.
 * Idempotent; cheap to call on every volume step. */
void g90_local_af_set(bool on, int gain_pct);
void g90_local_af_stop(void);
