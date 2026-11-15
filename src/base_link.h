/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - whether a body on its cable answers (BOARD_BASE_DETACHABLE)
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#pragma once

#include "board.h"
#include "lvgl/lvgl.h"

#ifdef BOARD_BASE_DETACHABLE
/* From the main screen's build(), again after a theme change: the link box between the S-meter and the clock (VISION:
 * at the right end of the S-meter's row; deck: a line on the top row, left of the status icons), or a line in the
 * S-meter where the theme leaves no room; while there is no body, a shade over the spectrum and the waterfall that
 * says so */
void base_link_build(lv_obj_t *screen, lv_obj_t *meter, lv_obj_t *spectrum, lv_obj_t *waterfall);
#endif
