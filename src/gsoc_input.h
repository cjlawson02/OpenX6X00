/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GSOC front panel: one LM8323 evdev for every key and knob, and the GT911
 *  touchscreen. board_input_open() is what gui_main.c calls when BOARD_INPUT_CUSTOM is set
 *  (cmake -DBOARD_GSOC=ON). The X6100's five event nodes are not used.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* GUI thread, once at start. Missing devices are logged; the GUI still comes up. */
void board_input_open(void);

/* The remote's keys and knobs (remote_server.c, BOARD_INPUT_CUSTOM): the node is grabbed, nothing can be written
 * into it, so they are handled as the panel's own on the LVGL thread, but not taken for the operator at the radio.
 * A key by its panel code; a knob by name (main, vol, sql, bw, cnt), n detents, positive clockwise: false for an
 * unknown one. Any thread */
void board_input_key(uint16_t code, bool down);
bool board_input_knob(const char *name, int n);
