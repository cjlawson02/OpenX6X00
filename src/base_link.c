/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - whether a body on its cable answers (BOARD_BASE_DETACHABLE)
 *
 *  The GSOC's body is on a DE-9 cable: the GUI starts without it (radio_init) and goes on when it is unplugged.
 *  Disconnected NO_DATA_MS after its last packet (radio_no_data), and from the start until the first one. On the GUI
 *  thread, by a timer: radio_no_data reads the radio thread's packet time atomically.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#include "base_link.h"

#ifdef BOARD_BASE_DETACHABLE

#include "gui_styles.h"
#include "radio.h"
#include "theme.h"

#define POLL_MS     250
#define BOX_MIN_W   120     /* narrower: the line in the S-meter */

/* The classic top bar keeps the S-meter at its 800 px width: at 1024 px 221 px are left before the clock */
#define BOX_X       (TOP_BAR_METER_X + TOP_BAR_METER_W + TOP_BAR_INNER_GAP)
#define BOX_W       (BOARD_DISP_W - TOP_BAR_MARGIN - CLOCK_W - TOP_BAR_INNER_GAP - BOX_X)

/* VISION: the S-meter's row is full, the meter to the battery box, its bars and scale over the whole of it: the box at
 * the meter's right end, the meter that much narrower ("Connected to G90" is 179 px in sony_24) */
#define VISION_BOX_W    200
#define VISION_METER_W  (BOARD_DISP_W - 3 * TOP_BAR_MARGIN - VISION_POWER_W)   /* gui_styles.c */

/* Deck: the S-meter's rows fill its box; on the top row instead, between the indicators (XIT ends near x 470) and
 * the status icons (right-aligned 104 px in, six at most: none left of x 760) */
#define DECK_RIGHT      284     /* the line's right end, from the screen's: x 740 at 1024 px */

static lv_style_t  box_style;
static lv_obj_t   *label;
static lv_timer_t *timer;
static int8_t      shown = -1;  /* -1: not shown yet on these widgets */

/* A top-bar box of the theme, as walkie.c makes its own */
static lv_obj_t *box_make(lv_obj_t *screen, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *box = lv_obj_create(screen);

    lv_obj_remove_style_all(box);
    styles_theme_box(&box_style, TC_bar, TC_bar_grad, TC_bar_border, theme_shape.radius, TI_bar, w, h);
    lv_obj_add_style(box, &box_style, 0);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static void poll(lv_timer_t *t) {
    int8_t up = !radio_no_data();

    (void)t;
    if (up == shown) return;
    shown = up;
    lv_label_set_text(label, up ? "Connected to G90" : "G90 disconnected");
    lv_obj_set_style_text_color(label, up ? TC(ok) : TC(warn), 0);
}

/* Again on a theme change, on new widgets (the old ones gone with lv_obj_clean): the timer is made once */
void base_link_build(lv_obj_t *screen, lv_obj_t *meter) {
    lv_obj_t *box = NULL;

    if (theme_shape.vision) {
        lv_coord_t w = VISION_METER_W - VISION_BOX_W - TOP_BAR_MARGIN;

        lv_obj_set_width(meter, w);
        box = box_make(screen, 2 * TOP_BAR_MARGIN + w, FREQ_STRIP_Y + FREQ_STRIP_H + TOP_BAR_MARGIN, VISION_BOX_W,
                       METER_ROW_H - 2 * TOP_BAR_MARGIN);
    } else if (!theme_shape.deck && BOX_W >= BOX_MIN_W) {
        box = box_make(screen, BOX_X, TOP_BAR_MARGIN, BOX_W, TOP_BAR_BOX_H);
    }
    if (box) {
        label = lv_label_create(box);
        lv_obj_set_style_text_font(label, &sony_24, 0);
        lv_obj_center(label);
    } else if (theme_shape.deck) {
        label = lv_label_create(screen);
        lv_obj_set_style_text_font(label, &sony_24, 0);
        lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -DECK_RIGHT, 2);
    } else {
        /* bottom left in the box, under the bars and left of the value */
        label = lv_label_create(meter);
        lv_obj_set_style_text_font(label, &sony_16, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    }
    shown = -1;
    poll(NULL);
    if (!timer) timer = lv_timer_create(poll, POLL_MS, NULL);
}

#endif
