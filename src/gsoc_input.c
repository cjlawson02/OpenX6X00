/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GSOC keys, knobs and touch.
 *
 *  The LM8323 is one evdev node. Knobs are key detents (press 1, release 0), not EV_REL.
 *  The node is grabbed so KEY_UP from the main knob does not move LVGL focus.
 *  The GT911 is a second node, a pointer. Names come from the kernel ("lm8323", "goodix"),
 *  not from event numbers, which move between boots.
 *
 *  What each silk key does, against the GUI that already exists:
 *    MAIN            tunes, one detent one step (EVENT_ROTARY)
 *    VOL inner       the VOL value (vol_update)
 *    VOL outer       AGC, one step per detent (the GUI only has "next")
 *    VOL push        frequency lock, as a tap. A hold is not forwarded: KEYPAD_LOCK's hold
 *                    resets the radio, and this key is not that
 *    left inner      filter bandwidth (HI/BW)
 *    left outer      filter center, both edges by the same step (LOW/CNT)
 *    left push       mode, as the hand mic MODE key
 *    F               RIT +10 Hz; hold is XIT +10 Hz (the setting exists; the DE-9 frame has no XIT)
 *    Po              transmit power +1 W; hold is RF gain +1 (the setting exists; the DE-9 frame has no RF gain)
 *    ATU             KEYPAD_ATU (tap toggles, hold tunes)
 *    user keys       noise blanker, split, compressor
 *    hand-mic PTT    KEYPAD_PTT
 *    the other mic keys  the X6100 mic codes (hkey_put), so the mic screens already handle them
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#define _GNU_SOURCE

#include "gsoc_input.h"

#include "board.h"
#include "front_keys.h"
#include "gui_events.h"
#include "knob_device.h"
#include "mic_keys.h"
#include "radio_types.h"
#include "screen_light.h"
#include "screen_main.h"
#include "vol_control.h"
#include "control_owner.h"
#include "radio_controls.h"
#include "lvgl/lvgl.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GSOC_INPUT_SCAN 32

enum {
    ACT_NONE = 0,
    ACT_TUNE,       /* dir +1 / -1 */
    ACT_VOL,        /* dir */
    ACT_AGC,        /* one step, either detent */
    ACT_MFK,        /* dir, or 0 for the press */
    ACT_BW,
    ACT_CENTER,
    ACT_RIT,        /* tap RIT, hold XIT */
    ACT_PWR,        /* tap power, hold RF gain */
    ACT_NB,
    ACT_SPLIT,
    ACT_COMP,
    ACT_LOCK_TAP,
    ACT_ATU,
    ACT_PTT,
    ACT_HKEY        /* radio_types.h RADIO_HKEY_* */
};

static const struct {
    uint16_t code;
    int8_t   dir;
    uint8_t  act;
    uint8_t  hkey;
} map[] = {
    {KEY_UP,         +1, ACT_TUNE, 0},
    {KEY_DOWN,       -1, ACT_TUNE, 0},
    {KEY_VOLUMEUP,   +1, ACT_VOL,  0},
    {KEY_VOLUMEDOWN, -1, ACT_VOL,  0},
    {KEY_F18,         0, ACT_AGC,  0},            /* VOL outer CW */
    {KEY_F19,         0, ACT_AGC,  0},            /* VOL outer CCW: the GUI cycles one way */
    {KEY_F13,        +1, ACT_BW,     0},          /* HI/BW inner */
    {KEY_F14,        -1, ACT_BW,     0},
    {KEY_F15,        +1, ACT_CENTER, 0},          /* LOW/CNT outer */
    {KEY_F16,        -1, ACT_CENTER, 0},
    {KEY_F17,         0, ACT_HKEY, RADIO_HKEY_MODE}, /* filter push: mode cycle */
    {KEY_KBDILLUMTOGGLE, 0, ACT_RIT, 0},          /* F / delta-F */
    {KEY_KBDILLUMDOWN,   0, ACT_PWR, 0},          /* Po */
    {KEY_SEND,           0, ACT_NB, 0},           /* user triangle */
    {KEY_REPLY,          0, ACT_SPLIT, 0},        /* user square */
    {KEY_FORWARDMAIL,    0, ACT_COMP, 0},         /* user circle */
    {KEY_F20,         0, ACT_LOCK_TAP, 0},        /* VOL push */
    {KEY_KBDILLUMUP,  0, ACT_ATU,  0},
    {KEY_PLAYPAUSE,   0, ACT_PTT,  0},
    {KEY_INSERT,      0, ACT_LOCK_TAP, 0},        /* mic LOCK */
    {KEY_PAGEUP,      0, ACT_HKEY, RADIO_HKEY_UP},
    {KEY_PAGEDOWN,    0, ACT_HKEY, RADIO_HKEY_DOWN},
    {KEY_DELETE,      0, ACT_HKEY, RADIO_HKEY_TUNER},
    {KEY_HOME,        0, ACT_HKEY, RADIO_HKEY_VM},
    {KEY_END,         0, ACT_HKEY, RADIO_HKEY_NW},
    {KEY_XFER,        0, ACT_HKEY, RADIO_HKEY_XFC},
    {KEY_F1,          0, ACT_HKEY, RADIO_HKEY_F1},
    {KEY_F2,          0, ACT_HKEY, RADIO_HKEY_F2},
    {KEY_F3,          0, ACT_HKEY, RADIO_HKEY_MODE},
    {KEY_F4,          0, ACT_HKEY, RADIO_HKEY_FIL},
    {KEY_F5,          0, ACT_HKEY, RADIO_HKEY_GENE},
    {KEY_1,           0, ACT_HKEY, RADIO_HKEY_1},
    {KEY_2,           0, ACT_HKEY, RADIO_HKEY_2},
    {KEY_3,           0, ACT_HKEY, RADIO_HKEY_3},
    {KEY_4,           0, ACT_HKEY, RADIO_HKEY_4},
    {KEY_5,           0, ACT_HKEY, RADIO_HKEY_5},
    {KEY_6,           0, ACT_HKEY, RADIO_HKEY_6},
    {KEY_7,           0, ACT_HKEY, RADIO_HKEY_7},
    {KEY_8,           0, ACT_HKEY, RADIO_HKEY_8},
    {KEY_9,           0, ACT_HKEY, RADIO_HKEY_9},
    {KEY_0,           0, ACT_HKEY, RADIO_HKEY_0},
    {KEY_DOT,         0, ACT_HKEY, RADIO_HKEY_DOT},
    {KEY_BACKSPACE,   0, ACT_HKEY, RADIO_HKEY_CE},
    {KEY_ENTER,       0, ACT_HKEY, RADIO_HKEY_FINP},
};

static int keys_fd = -1;
static int touch_fd = -1;
static int touch_x, touch_y;
static int touch_down;
static struct input_absinfo abs_x, abs_y;

/* First /dev/input/eventN whose kernel name contains needle. grab keeps it from the rest of
 * userspace. The path is copied to out (out_len). -1 if none. */
static int open_named(const char *needle, bool grab, char *out, size_t out_len) {
    for (int i = 0; i < GSOC_INPUT_SCAN; i++) {
        char path[64], name[128];
        int  fd;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        name[0] = '\0';
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 || !strcasestr(name, needle)) {
            close(fd);
            continue;
        }
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            perror("gsoc input: grab");
        }
        snprintf(out, out_len, "%s", path);
        return fd;
    }
    return -1;
}

/* F and Po: the action at release if the key was let go inside a second, or at the second if held. */
enum { HOLD_RIT = 0, HOLD_PWR = 1 };

static struct {
    lv_timer_t *timer;
    bool        down;
    bool        long_sent;
} held[2];

static void held_cb(lv_timer_t *t) {
    int which = (int)(intptr_t)t->user_data;

    held[which].timer     = NULL;
    held[which].long_sent = true;
    backlight_tick();
    if (which == HOLD_RIT) {
        controls_turn(CTRL_XIT, 1);
    } else {
        controls_turn(CTRL_RFG, 1);
    }
}

static void tap_rit(void) {
    controls_turn(CTRL_RIT, 1);
}

static void tap_pwr(void) {
    controls_turn(CTRL_PWR, 10);    /* the power step is 0.1 W */
}

static void hold_key(int which, int value, void (*tap)(void)) {
    if (held[which].timer) {
        lv_timer_del(held[which].timer);
        held[which].timer = NULL;
    }
    if (value == 1) {
        held[which].down      = true;
        held[which].long_sent = false;
        held[which].timer     = lv_timer_create(held_cb, 1000, (void *)(intptr_t)which);
        lv_timer_set_repeat_count(held[which].timer, 1);
    } else if (held[which].down) {
        held[which].down = false;
        if (!held[which].long_sent) {
            backlight_tick();
            tap();
        }
    }
}

static void tune(int dir) {
    rotary_data_t *ev = malloc(sizeof(*ev));

    if (!ev) {
        return;
    }
    ev->diff = (int16_t)dir;
    ev->dt   = 1;
    backlight_tick();
    lv_event_send(lv_scr_act(), EVENT_ROTARY, ev);   /* main_knob_cb frees ev */
}

static void on_key(const struct input_event *in) {
    size_t i;

    if (in->value == 2) {
        return;     /* autorepeat: a detent is the press, not a held key */
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].code == in->code) {
            break;
        }
    }
    if (i == sizeof(map) / sizeof(map[0])) {
        if (in->value == 1) {
            LV_LOG_USER("gsoc key %u has no action", (unsigned)in->code);
        }
        return;
    }
    if (in->value == 1) {
        ctl_local_input();
    }

    switch (map[i].act) {
    case ACT_TUNE:
        if (in->value == 1) {
            tune(map[i].dir);
        }
        break;
    case ACT_VOL:
        if (in->value == 1) {
            backlight_tick();
            vol_update(map[i].dir);
        }
        break;
    case ACT_AGC:
        if (in->value == 1) {
            backlight_tick();
            main_screen_agc_next();
        }
        break;
    case ACT_MFK:
        if (in->value == 1) {
            backlight_tick();
            main_screen_mfk(map[i].dir);
        }
        break;
    case ACT_BW:
        if (in->value == 1) {
            backlight_tick();
            controls_turn(CTRL_FILTER_BW, map[i].dir);
        }
        break;
    case ACT_CENTER:
        if (in->value == 1) {
            backlight_tick();
            controls_filter_center(map[i].dir);
        }
        break;
    case ACT_RIT:
        hold_key(HOLD_RIT, in->value, tap_rit);
        break;
    case ACT_PWR:
        hold_key(HOLD_PWR, in->value, tap_pwr);
        break;
    case ACT_NB:
        if (in->value == 1) {
            backlight_tick();
            controls_turn(CTRL_NB, 1);
        }
        break;
    case ACT_SPLIT:
        if (in->value == 1) {
            backlight_tick();
            controls_split_toggle();
        }
        break;
    case ACT_COMP:
        if (in->value == 1) {
            backlight_tick();
            controls_comp_toggle();
        }
        break;
    case ACT_LOCK_TAP:
        if (in->value == 1) {
            keypad_button(KEYPAD_LOCK, 1);
            keypad_button(KEYPAD_LOCK, 0);   /* before the hold, which would reset the radio */
        }
        break;
    case ACT_ATU:
        keypad_button(KEYPAD_ATU, in->value == 1 ? 1 : 0);
        break;
    case ACT_PTT:
        keypad_button(KEYPAD_PTT, in->value == 1 ? 1 : 0);
        break;
    case ACT_HKEY:
        hkey_put(in->value == 1 ? map[i].hkey : 0);
        break;
    default:
        break;
    }
}

static void keys_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    struct input_event in;

    (void)drv;
    data->continue_reading = false;
    data->key              = 0;
    data->state            = LV_INDEV_STATE_RELEASED;
    while (read(keys_fd, &in, sizeof(in)) == (ssize_t)sizeof(in)) {
        data->continue_reading = true;
        if (in.type == EV_KEY) {
            on_key(&in);
        }
    }
    /* Nothing is handed to LVGL as a key: KEY_UP would move the focus. */
}

/* The GT911's coordinates onto the panel. absinfo is the kernel's range. */
static lv_coord_t axis_to_px(int v, const struct input_absinfo *ax, lv_coord_t pixels) {
    int span = ax->maximum - ax->minimum;

    if (span <= 0) {
        return 0;
    }
    if (v < ax->minimum) {
        v = ax->minimum;
    }
    if (v > ax->maximum) {
        v = ax->maximum;
    }
    return (lv_coord_t)((int64_t)(v - ax->minimum) * (pixels - 1) / span);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    struct input_event in;

    (void)drv;
    data->continue_reading = false;
    while (read(touch_fd, &in, sizeof(in)) == (ssize_t)sizeof(in)) {
        data->continue_reading = true;
        if (in.type == EV_ABS && in.code == ABS_X) {
            touch_x = in.value;
        } else if (in.type == EV_ABS && in.code == ABS_Y) {
            touch_y = in.value;
        } else if (in.type == EV_KEY && (in.code == BTN_TOUCH || in.code == BTN_LEFT)) {
            touch_down = in.value != 0;
        }
    }
    data->point.x = axis_to_px(touch_x, &abs_x, BOARD_DISP_W);
    data->point.y = axis_to_px(touch_y, &abs_y, BOARD_DISP_H);
    data->state   = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void board_input_open(void) {
    static lv_indev_drv_t keys_drv, touch_drv;
    char path[64];

    keys_fd = open_named("lm8323", true, path, sizeof(path));
    if (keys_fd < 0) {
        LV_LOG_ERROR("gsoc: no LM8323 input device");
    } else {
        LV_LOG_USER("gsoc keys: %s", path);
        lv_indev_drv_init(&keys_drv);
        keys_drv.type    = LV_INDEV_TYPE_KEYPAD;
        keys_drv.read_cb = keys_read;
        lv_indev_drv_register(&keys_drv);
    }

    touch_fd = open_named("goodix", false, path, sizeof(path));
    if (touch_fd < 0) {
        LV_LOG_ERROR("gsoc: no GT911 touch device");
        return;
    }
    if (ioctl(touch_fd, EVIOCGABS(ABS_X), &abs_x) < 0 || ioctl(touch_fd, EVIOCGABS(ABS_Y), &abs_y) < 0) {
        LV_LOG_ERROR("gsoc: touch has no absolute axes");
        close(touch_fd);
        touch_fd = -1;
        return;
    }
    LV_LOG_USER("gsoc touch: %s", path);
    lv_indev_drv_init(&touch_drv);
    touch_drv.type             = LV_INDEV_TYPE_POINTER;
    touch_drv.read_cb          = touch_read;
    touch_drv.long_press_time  = 1000;
    lv_indev_drv_register(&touch_drv);
}
