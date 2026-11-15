/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - whether a body on its cable answers (BOARD_BASE_DETACHABLE)
 *
 *  The GSOC's body is on a DE-9 cable: the GUI starts without it (radio_init) and goes on when it is unplugged.
 *  Disconnected NO_DATA_MS after its last packet (radio_no_data), and from the start until the first one. On the GUI
 *  thread, by a timer: radio_no_data reads the radio thread's packet time atomically.
 *
 *  Without it the dial, the band, the mode and A/B are held as an app holds them (main_screen_board_hold: the knobs,
 *  the keyboard, touch tuning and the remote already respect that), and the volume (radio_hold_vol): a turn with
 *  nothing to hear it by made the body loud when it came back. What else is set is kept and goes to the body with
 *  its first packet: each exchange carries the whole state. TX is refused meanwhile (no_data_refused). Before the
 *  first packet it is held too, but "Waiting for G90" for NO_DATA_MS: a body there answers in well under it.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#include "base_link.h"

#ifdef BOARD_BASE_DETACHABLE

#include "base_guard.h"
#include "button_bar.h"
#include "gui_styles.h"
#include "radio.h"
#include "screen_main.h"
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
static lv_obj_t   *shade;   /* over the spectrum and the waterfall, which stand still without the body */
static lv_obj_t   *spectrum;
static lv_timer_t *timer;
static uint32_t    start;       /* the first build: the GUI's start */
static bool        ever_up;     /* a packet came */
static bool        held;
static int8_t      shown = -1;  /* LINK_*, -1: not shown yet on these widgets */

enum { LINK_WAIT, LINK_UP, LINK_AWAY };

/* From the spectrum's top (with the spectrum off, under the strip it hides behind) to the buttons, or the bottom
 * with them hidden: again on each poll, the layout may change while it is shown */
static void shade_place(void) {
    lv_coord_t top = LV_MAX(lv_obj_get_y(spectrum), FREQ_STRIP_Y + FREQ_STRIP_H + METER_ROW_H);
    lv_coord_t end = buttons_hidden() ? BOARD_DISP_H : BOARD_DISP_H - BUTTONS_H;

    lv_obj_set_pos(shade, 0, top);
    lv_obj_set_size(shade, BOARD_DISP_W, end - top);
}

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
    bool   up = !radio_no_data();
    int8_t link;

    (void)t;
    ever_up |= up;
    link = up ? LINK_UP : ever_up || lv_tick_elaps(start) >= NO_DATA_MS ? LINK_AWAY : LINK_WAIT;
    if (held != !up) {
        held = !up;
        main_screen_board_hold(held);
        radio_hold_vol(held);
    }
    if (link == LINK_AWAY) shade_place();
    if (link == shown) return;
    shown = link;
    lv_label_set_text(label, link == LINK_UP ? "Connected to G90" : link == LINK_AWAY ? "G90 disconnected" : "Waiting for G90");
    lv_obj_set_style_text_color(label, link == LINK_UP ? TC(ok) : TC(warn), 0);
    if (link == LINK_AWAY) {
        lv_obj_clear_flag(shade, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Over the waterfall and what lies on it, under the panels, the buttons and the messages. Not clickable: a touch
 * goes to what is under it */
static void shade_build(lv_obj_t *screen, lv_obj_t *waterfall) {
    lv_obj_t *text;

    shade = lv_obj_create(screen);
    lv_obj_move_to_index(shade, lv_obj_get_index(waterfall) + 1);
    lv_obj_remove_style_all(shade);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
    lv_obj_clear_flag(shade, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);
    text = lv_label_create(shade);
    lv_obj_set_style_text_font(text, &sony_28, 0);
    lv_obj_set_style_text_color(text, TC(warn), 0);
    lv_label_set_text(text, "G90 disconnected (DE-9)");
    lv_obj_center(text);
}

/* Again on a theme change, on new widgets (the old ones gone with lv_obj_clean): the timer is made once */
void base_link_build(lv_obj_t *screen, lv_obj_t *meter, lv_obj_t *spectrum_obj, lv_obj_t *waterfall) {
    lv_obj_t *box = NULL;

    spectrum = spectrum_obj;
    shade_build(screen, waterfall);
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
    if (!timer) {
        start = lv_tick_get();
        timer = lv_timer_create(poll, POLL_MS, NULL);
    }
    poll(NULL);
}

#endif
