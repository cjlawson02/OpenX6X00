/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - two axes: HW_BACKEND picks the radio behind hw/hw.h; this file picks the panel
 *  (size, input nodes, audio names). HW_BACKEND=g90 does not change the panel. A future GSOC panel
 *  is BOARD_GSOC, not BOARD_X6200.
 *  The X6100 unless the build says BOARD_X6200 (cmake -DHW_BACKEND=x6200). Another panel on the
 *  same code changes this file, not the places that use the macros.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

/* The panel, landscape. BOARD_GSOC (cmake -DBOARD_GSOC=ON): 1024×600, landscape
 * framebuffer, board_input_open() instead of the five X6100 event nodes.
 * HW_BACKEND=g90 does not select it. BOARD_FB_ROTATE_90=0 uses fbdev's plain
 * copy; DRM still needs 1 and falls back to fbdev when it is 0. */
#ifdef BOARD_GSOC
#define BOARD_DISP_W        1024
#define BOARD_DISP_H        600
#define BOARD_FB_ROTATE_90  0
#define BOARD_INPUT_CUSTOM  1
#else
#define BOARD_DISP_W        800
#define BOARD_DISP_H        480
#endif

#ifndef BOARD_FB_ROTATE_90
#define BOARD_FB_ROTATE_90  1   /* The X6100/X6200 framebuffer is portrait; LVGL is landscape. */
#endif

#ifndef BOARD_G90_DEV
#define BOARD_G90_DEV       "/dev/ttyS1"
#endif
#ifndef BOARD_G90_PROFILE
#define BOARD_G90_PROFILE   "1.81"
#endif
#ifndef BOARD_G90_ALLOW_TX
#define BOARD_G90_ALLOW_TX  0
#endif

/* The AF volume's range, the base's own: the X6200 takes 0..100 (gdyuldin's X6200 GUI, 0d504863), the X6100 0..55 */
#ifdef BOARD_X6200
#define BOARD_VOL_MAX       100
#else
#define BOARD_VOL_MAX       55
#endif

/* The CW sidetone level's range (KEY > Volume): the X6200's base takes 0..63, a 6-bit field, and
 * gdyuldin's X6200 GUI goes that far (#110); the X6100 stays at 0..32, as it always was */
#ifdef BOARD_X6200
#define BOARD_KEY_VOL_MAX   63
#else
#define BOARD_KEY_VOL_MAX   32
#endif

/* Radio ranges + knob/encoder steps (radio_controls.cpp). HW_BACKEND picks the body; each
 * profile sets every macro — do not fall through to another board's steps. */
#ifdef BOARD_X6200
#define BOARD_PWR_MIN           0.1f
#define BOARD_PWR_MAX           8.0f
#define BOARD_PWR_MAX_BAT       5.0f
#define BOARD_PWR_STEP          0.1f
#define BOARD_PWR_LABEL_FMT     "%0.1f"
#define BOARD_SQL_MAX           100
#define BOARD_RFG_MAX           100
#define BOARD_RIT_MAX           1500
#define BOARD_XIT_MAX           1500
#define BOARD_MIC_MAX           50
#define BOARD_NB_MAX            100
#define BOARD_HF_MIN_HZ         100000
#define BOARD_HF_MAX_HZ         55000000
#define BOARD_SQL_STEP          1
#define BOARD_RFG_STEP          1
#define BOARD_RIT_STEP          10
#define BOARD_XIT_STEP          10
#define BOARD_NB_LEVEL_STEP     5
#define BOARD_NB_WIDTH_STEP     5
#define BOARD_NR_LEVEL_STEP     5
#define BOARD_MONI_STEP         1
#define BOARD_KEY_SPEED_STEP    1
#define BOARD_KEY_TONE_STEP     10
#define BOARD_KEY_VOL_STEP      1
#define BOARD_QSK_TIME_STEP     10
#define BOARD_KEY_RATIO_STEP    0.1f
#define BOARD_CW_DECODER_SNR_STEP 0.1f
#define BOARD_CW_PEAK_Q_STEP    1
#define BOARD_DNF_CENTER_STEP   50
#define BOARD_DNF_WIDTH_STEP    5
#define BOARD_WF_REF_STEP       1
#define BOARD_FILTER_LOW_STEP   10
#define BOARD_FILTER_BW_STEP    20
#elif defined(HW_BACKEND_G90)
#define BOARD_PWR_MIN           1.0f
#define BOARD_PWR_MAX           20.0f
#define BOARD_PWR_STEP          1.0f        /* head rf_power uint8_t watts */
#define BOARD_PWR_LABEL_FMT     "%0.0f"
#define BOARD_SQL_MAX           9           /* head sql_level; Radioddity S1–S9 */
#define BOARD_MIC_MAX           20          /* head mic_gain; Radioddity 0–20 */
#define BOARD_RIT_MAX           512         /* head 10-bit ritval −512…511; UX ±500 Hz */
#define BOARD_HF_MIN_HZ         500000      /* Radioddity RX 0.5–30 MHz */
#define BOARD_HF_MAX_HZ         30000000
/* Still X6100 UI scale until SoT is wired (RFG / XIT / NB max) */
#define BOARD_RFG_MAX           100
#define BOARD_XIT_MAX           1500
#define BOARD_NB_MAX            100
#define BOARD_SQL_STEP          1
#define BOARD_RFG_STEP          1
#define BOARD_RIT_STEP          10
#define BOARD_XIT_STEP          10
#define BOARD_NB_LEVEL_STEP     1           /* Xiegu cheat sheet NB Level/Width 0–10 */
#define BOARD_NB_WIDTH_STEP     1
#define BOARD_NR_LEVEL_STEP     5
#define BOARD_MONI_STEP         1
#define BOARD_KEY_SPEED_STEP    1
#define BOARD_KEY_TONE_STEP     10
#define BOARD_KEY_VOL_STEP      1
#define BOARD_QSK_TIME_STEP     10
#define BOARD_KEY_RATIO_STEP    0.1f
#define BOARD_CW_DECODER_SNR_STEP 0.1f
#define BOARD_CW_PEAK_Q_STEP    1
#define BOARD_DNF_CENTER_STEP   50
#define BOARD_DNF_WIDTH_STEP    5
#define BOARD_WF_REF_STEP       1
#define BOARD_FILTER_LOW_STEP   10
#define BOARD_FILTER_BW_STEP    20
#else /* X6100 */
#define BOARD_PWR_MIN           0.1f
#define BOARD_PWR_MAX           10.0f
#define BOARD_PWR_STEP          0.1f
#define BOARD_PWR_LABEL_FMT     "%0.1f"
#define BOARD_SQL_MAX           100
#define BOARD_RFG_MAX           100
#define BOARD_RIT_MAX           1500
#define BOARD_XIT_MAX           1500
#define BOARD_MIC_MAX           50
#define BOARD_NB_MAX            100
#define BOARD_HF_MIN_HZ         100000      /* upstream f6c97fa */
#define BOARD_HF_MAX_HZ         55000000
#define BOARD_SQL_STEP          1
#define BOARD_RFG_STEP          1
#define BOARD_RIT_STEP          10
#define BOARD_XIT_STEP          10
#define BOARD_NB_LEVEL_STEP     5
#define BOARD_NB_WIDTH_STEP     5
#define BOARD_NR_LEVEL_STEP     5
#define BOARD_MONI_STEP         1
#define BOARD_KEY_SPEED_STEP    1
#define BOARD_KEY_TONE_STEP     10
#define BOARD_KEY_VOL_STEP      1
#define BOARD_QSK_TIME_STEP     10
#define BOARD_KEY_RATIO_STEP    0.1f
#define BOARD_CW_DECODER_SNR_STEP 0.1f
#define BOARD_CW_PEAK_Q_STEP    1
#define BOARD_DNF_CENTER_STEP   50
#define BOARD_DNF_WIDTH_STEP    5
#define BOARD_WF_REF_STEP       1
#define BOARD_FILTER_LOW_STEP   10
#define BOARD_FILTER_BW_STEP    20
#endif

/* The low-battery power-off (bat_watch.c), the battery's voltage in 0.1 V: from there "Low battery! Turning off in
 * 30s.", then the charger set back On, the save and the radio off. The X6100 runs to 6.0 V, as it always did. The
 * X6200's base (1.0.8) cuts the power by itself at about 6.4-6.6 V, with nothing saved and the run's log lost: in
 * #160, on receive with WiFi and Bluetooth on, the first 6.8 V reading came 4-5 min before its cut, 6.7 V about
 * 2.5 min. At 6.8 V (v0.3.9) it went off in order on #160's radio and twice on the bench's, but the bench's behaves
 * erratically near the end of its battery, so 7.0 V: on receive about 3 min sooner (#160's run: 7.0 V at 00:14, 6.8 V
 * at 00:17), more for a TX at the end, which brings the cut nearer. A TX that sags the battery to 7.0 V starts it
 * too, and it is called off only at BAT_HIGH, 7.2 V: so with 7.1 V at rest a TX ends the run (#160's run: 7.2 V
 * half an hour before its power-off).
 * BOARD_BAT_EXT_MIN: on the X6200 only DC IN that the base reads at 12.0 V and more stops it (with hysteresis,
 * bat_watch.c), its manual's least for charging (12-13.8 V). Xiegu's 12 V 1 A charger reads 11.7-11.8 V, so it is
 * under. In #160 a charger at 11.6-11.8 V did not hold a flat battery up: with the radio on it fell from 6.9 to 6.4 V
 * in 2 minutes and the base cut the power again, its flags saying external power all the same (its charging flag
 * comes with any supply, radio.c). With the battery at 7.7-7.8 V the stock charger ran the radio (#33). On the X6100
 * any external power stops it, as before */
#ifdef BOARD_X6200
#define BOARD_BAT_LOW       70
#define BOARD_BAT_EXT_MIN   120
#else
#define BOARD_BAT_LOW       60
#endif

#ifdef HOST_BUILD
/* The GUI on a PC (cmake -DHOST_BUILD=ON): an SDL window for the panel, the stub radio, and the
 * front keys and knobs as FIFOs under the data directory that host/host_panel.c writes the PC's
 * keys and mouse wheel into, in the devices' own event format (so the drivers and the browser
 * panel's key injection are the radio's). BOARD_DATA and BOARD_SHARE come from the build */
#define BOARD_DEV_KEYPAD    BOARD_DATA "/run/keypad"
#define BOARD_DEV_MAIN      BOARD_DATA "/run/main"
#define BOARD_DEV_VOL       BOARD_DATA "/run/vol"
#define BOARD_DEV_MFK       BOARD_DATA "/run/mfk"
#define BOARD_DEV_POWER     BOARD_DATA "/run/power"
#define BOARD_PA_SINK       NULL                    /* PulseAudio's defaults */
#define BOARD_PA_SOURCE     NULL
#define BOARD_MIXER_PLAY    NULL                    /* no codec levels to set */
#define BOARD_MIXER_REC     NULL
#define BOARD_CAT_DEV       BOARD_DATA "/run/cat"   /* none: X6100_CAT_DEV=<pty> gives one */
#define BOARD_RTC_DEV       "/dev/null"
#define BOARD_NAME          "host"
#define BOARD_NAME_LOWER    "x6100"
#else

#ifdef BOARD_GSOC
/* GSOC panel: LM8323 + GT911 via board_input_open() (BOARD_INPUT_CUSTOM). No X6100
 * event nodes. CS4271 (card GSOC-Sound-Card): kernel cs4271.c "Master Playback Volume";
 * capture is switch-only (no dB selem). SoC RTC is rtc0 on the A20. */
#define BOARD_NAME          "GSOC"
#define BOARD_NAME_LOWER    "gsoc"
#define BOARD_DEV_KEYPAD    NULL
#define BOARD_DEV_MAIN      NULL
#define BOARD_DEV_VOL       NULL
#define BOARD_DEV_MFK       NULL
#define BOARD_DEV_POWER     NULL
#define BOARD_PA_SINK       "alsa_output.platform-sound.stereo-fallback"
#define BOARD_PA_SOURCE     "alsa_input.platform-sound.stereo-fallback"
#define BOARD_MIXER_PLAY    "Master"
#define BOARD_MIXER_REC     NULL
#define BOARD_CAT_DEV       "/dev/ttyS2"
#define BOARD_RTC_DEV       "/dev/rtc0"
#elif defined(BOARD_X6200)
#define BOARD_NAME          "X6200"
#define BOARD_NAME_LOWER    "x6200"                 /* the default network name, what WSJT-X's protocol is told */
#define BOARD_DEV_KEYPAD    "/dev/input/event0"
#define BOARD_DEV_MAIN      "/dev/input/event1"
#define BOARD_DEV_VOL       "/dev/input/event2"
#define BOARD_DEV_MFK       "/dev/input/event3"
#define BOARD_DEV_MFK_INNER "/dev/input/event4"     /* the inner ring of the MFK knob: what the MFK controls */
#define BOARD_DEV_POWER     NULL                    /* the base reports the POWER key in its packets */
#define BOARD_PA_SINK       "alsa_output.platform-sound.stereo-fallback"
#define BOARD_PA_SOURCE     "alsa_input.platform-sound.stereo-fallback"
#define BOARD_MIXER_PLAY    "AIF1 DA0"
#define BOARD_MIXER_REC     "ADC"
#define BOARD_CAT_DEV       "/dev/ttyS2"
#define BOARD_RTC_DEV       "/dev/rtc1"
#else
/* X6100 panel */
#define BOARD_NAME          "X6100"
#define BOARD_NAME_LOWER    "x6100"
#define BOARD_DEV_KEYPAD    "/dev/input/event0"     /* the front keys, a matrix keypad */
#define BOARD_DEV_MAIN      "/dev/input/event1"     /* the main knob */
#define BOARD_DEV_VOL       "/dev/input/event2"
#define BOARD_DEV_MFK       "/dev/input/event3"
#define BOARD_DEV_POWER     "/dev/input/event4"     /* the PMIC's key: the matrix keypad drops KEY_POWER */
#define BOARD_PA_SINK       "alsa_output.platform-sound.stereo-fallback"
#define BOARD_PA_SOURCE     "alsa_input.platform-sound.stereo-fallback"
#define BOARD_MIXER_PLAY    "AIF1 DA0"
#define BOARD_MIXER_REC     "ADC"
#define BOARD_CAT_DEV       "/dev/ttyS2"
#define BOARD_RTC_DEV       "/dev/rtc1"
#endif

/* The user's data: the settings database, logs, macros, recordings, themes. The radio's data
 * partition; a desktop build passes its own directory (-DBOARD_DATA='"..."') */
#ifndef BOARD_DATA
#define BOARD_DATA          "/mnt"
#endif
#endif

/* What the image ships read-only: the default settings, icons, the country list, the bundled
 * themes and pictures */
#ifndef BOARD_SHARE
#define BOARD_SHARE         "/usr/share/x6100"
#endif
