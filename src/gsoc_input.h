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

/* GUI thread, once at start. Missing devices are logged; the GUI still comes up. */
void board_input_open(void);
