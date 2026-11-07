/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CS4271 capture (hw:1,0, S24 @ 192 kHz) → one fftshifted dB row.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include "g90_psd_fuse.h"

#include <stdbool.h>

/* Opens the codec and starts the capture thread. False if the device did not open
 * at 192 kHz (caller keeps the DE-9 spectrum). G90_IQ_DEV overrides the PCM name;
 * G90_IQ_SWAP=1 exchanges I and Q. */
bool g90_local_iq_start(void);
void g90_local_iq_stop(void);

/* Latest row, G90_FUSE_BINS, lowest frequency first. False until the first FFT. */
bool g90_local_iq_copy(float *db);
