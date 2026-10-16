/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/matrix.h>
#include <zmk/kscan.h>
#include <zmk/display.h>
#include <drivers/ext_power.h>

#include <zmk/hid.h>
#include <zmk/activity.h>
#include <dt-bindings/zmk/mouse.h>
#include <zmk/hid_indicators.h>
#include <zmk/indicator_capslock.h>
#ifdef CONFIG_ZMK_MOUSE
#include <zmk/mouse.h>
#endif /* CONFIG_ZMK_MOUSE */

// Trackpad scroll mode is hold-to-scroll: it is active while a key bound to
// the scroll-hold behavior (Select, on the uConsole keymap) is held down, as
// in the uConsole QMK firmware. This replaces fix9900's click-to-toggle on
// the trackpad's own press, which is a plain mouse click again. The same
// behavior has a `toggle` option for keymaps that want a scroll switch on a
// key instead; both come through zmk_trackpad_scroll_held().
//
// The held state is set from key events in behavior_scroll_hold.c and only
// read here, so this loop never polls key or modifier state itself. Polling
// modifier state from this thread is what raced against its blocking I2C
// reads in an earlier hold-Shift-to-scroll attempt.
#include <zmk/trackpad.h>

// fix9900: the trackpad-scroll condition, magnitude, and axis-lock math were
// all confirmed correct via live libinput/browser captures, but events
// were still being sent on essentially every ~20-25ms poll tick, 40+/sec.
// No human hand can spin a physical wheel that fast, and confirmed on real
// hardware: text editors and terminals apply each burst delta directly and
// "work" (if jumpy), while VS Code and Firefox, both of which run
// animated smooth-scroll interpolation tuned for human-paced discrete wheel
// clicks, receive the exact same valid events and never visibly move.
// Fixed by accumulating per-tick deltas here and only actually sending a
// scroll report every SCROLL_SEND_INTERVAL_MS, batched into one larger,
// human-plausible tick instead of a continuous flood of tiny ones.
#define SCROLL_SEND_INTERVAL_MS 80
// Motion is accumulated in hundredths of a scroll tick, so that movement too
// small to make a whole tick still counts towards the next one. Scroll speed
// then follows finger speed all the way down: an earlier version rounded any
// motion at all up to one tick per poll, which meant a slow finger scrolled
// no slower than a moderate one.
#define SCROLL_TICK_UNITS 100
#define SCROLL_MAX_TICKS_PER_SEND 5
static int32_t scroll_accum_x = 0;
static int32_t scroll_accum_y = 0;
// CONFIG_TRACKPAD_SCROLL_SPEED scales each batch, in percent. The part of a
// tick left over after scaling is carried into the next batch, not dropped.
// The remainders are in units of 1/(100 * SCROLL_TICK_UNITS) of a tick.
static int32_t scroll_rem_x = 0;
static int32_t scroll_rem_y = 0;

static int8_t scroll_scale(int32_t accum, int32_t *rem) {
    const int32_t whole = 100 * SCROLL_TICK_UNITS;
    int32_t scaled = accum * CONFIG_TRACKPAD_SCROLL_SPEED;

    // a leftover from the other direction would cancel the start of a
    // reversal, so it is only kept while the direction holds
    if ((scaled > 0 && *rem < 0) || (scaled < 0 && *rem > 0)) {
        *rem = 0;
    }
    scaled += *rem;
    *rem = scaled % whole;
    return (int8_t)CLAMP(scaled / whole, -127, 127);
}
static int64_t last_scroll_send_ms = 0;

static const struct device *get_a320_device(void) {
    const struct device *dev = DEVICE_DT_GET_ANY(avago_a320);

    if (dev == NULL) {
        printk("\nError: no device found.\n");
        return NULL;
    }
    if (!device_is_ready(dev)) {
        printk("\nError: Device \"%s\" is not ready; "
               "check the driver initialization logs for errors.\n",
               dev->name);
        return NULL;
    }
    printk("Found device \"%s\", getting sensor data\n", dev->name);
    return dev;
}
int main(void) {
    LOG_INF("Welcome to ZMK!\n");

    if (zmk_kscan_init(DEVICE_DT_GET(ZMK_MATRIX_NODE_ID)) != 0) {
        return -ENOTSUP;
    }
    const struct device *dev = get_a320_device();
    if (dev == NULL) {
        return;
    }
#ifdef CONFIG_ZMK_DISPLAY
    zmk_display_init();
#endif /* CONFIG_ZMK_DISPLAY */
    struct sensor_value xy_pos;
    while (1) {
        int sensor_read_rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &xy_pos);
        if (sensor_read_rc == 0 && zmk_trackpad_enabled() &&
            (xy_pos.val1 != 0 || xy_pos.val2 != 0)) {
            zmk_activity_notify();
        }
        char target_9900[] = "bb9900";
        char target_q10[] = "bbq10";
        char target_q20[] = "bbq20";
        char target_q30[] = "bbq30";
        char target_9981[] = "bbp9981";
        char target_9983[] = "bbp9983";
        char target_bbcase[] = "bbcase";
        if (strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_9900) == 0 ||
            strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_bbcase) == 0) {
            int8_t x = xy_pos.val2;
            int8_t y = xy_pos.val1;
            int8_t scroll_x = 0;
            int8_t scroll_y = 0;
            if (!zmk_trackpad_enabled()) {
                // Switched off from the keymap (the keyboard lock). The sensor
                // has still been read above, so no motion builds up to be
                // released in one jump when it is switched back on.
                x = 0;
                y = 0;
            }
            // fix9900: originally gated on CapsLock/ScrollLock HID indicator
            // state, which meant toggling CapsLock for normal typing
            // silently broke trackpad cursor movement. CapsLock is now a
            // plain modifier with zero side effects on the trackpad. Scroll
            // mode is now held from the keymap (see the note above the
            // trackpad.h include). Scoped to bb9900/bbcase only, not
            // touching the other board families' identical-looking blocks
            // below, no way to test them.
            if (zmk_trackpad_scroll_held()) {
                if (x != 0 || y != 0) {
                    // tells the held key it was used to scroll, so its
                    // release does not also send its tap
                    zmk_trackpad_scroll_mark_used();
                }
                // fix9900: the original vendor code picked a divisor from
                // |y| alone and applied it to BOTH x and y unconditionally.
                // That meant any sensor noise on the "wrong" axis during a
                // clean vertical or horizontal drag still got divided down
                // and rounded into a real, spurious scroll tick, which
                // showed up as horizontal scroll flickering during a
                // straight-up drag, confirmed via a live libinput capture.
                // It also produced huge single-report magnitudes (up to
                // +/-75 in one HID report), far outside what a real scroll
                // wheel (1 click per detent) ever sends, which is exactly
                // the kind of input well-behaved scroll-accumulation logic
                // in a compositor or app is liable to discard as noise.
                //
                // Real trackpads solve the first problem with axis
                // locking: within ~15 degrees of a cardinal direction,
                // commit to that axis only; blend both axes only in the
                // diagonal zone between locked directions. tan(15 deg) is
                // approximated as 27/100 to avoid pulling in floating point
                // trig for something this simple.
                const int abs_x = abs(x);
                const int abs_y = abs(y);
                const bool near_vertical = (abs_x * 100) <= (abs_y * 27);
                const bool near_horizontal = (abs_y * 100) <= (abs_x * 27);
                const int use_x = near_vertical ? 0 : x;
                const int use_y = near_horizontal ? 0 : y;

                const int magnitude = (abs_y > abs_x) ? abs_y : abs_x;
                int divisor;
                if (magnitude >= 128) {
                    divisor = 24;
                } else if (magnitude >= 64) {
                    divisor = 16;
                } else if (magnitude >= 32) {
                    divisor = 12;
                } else if (magnitude >= 21) {
                    divisor = 8;
                } else {
                    divisor = 4;
                }

                // in hundredths of a tick: nothing is rounded away here,
                // and nothing is rounded up to a whole tick either
                //
                // The two axes take opposite signs because the HID wheel and
                // pan usages point opposite ways: a positive wheel value is
                // up, a positive pan value is right. Moving a finger up
                // (negative y) scrolls up and moving it left (negative x)
                // scrolls left. Horizontal used to be negated like vertical,
                // which made it scroll the opposite way to the finger.
                scroll_accum_x += use_x * SCROLL_TICK_UNITS / divisor;
                scroll_accum_y += -use_y * SCROLL_TICK_UNITS / divisor;

                int64_t now_ms = k_uptime_get();
                if (now_ms - last_scroll_send_ms >= SCROLL_SEND_INTERVAL_MS) {
                    // clamp the batched total so a single HID report never
                    // sends more than a real scroll wheel plausibly would
                    const int32_t limit = SCROLL_MAX_TICKS_PER_SEND * SCROLL_TICK_UNITS;
                    scroll_accum_x = CLAMP(scroll_accum_x, -limit, limit);
                    scroll_accum_y = CLAMP(scroll_accum_y, -limit, limit);

                    scroll_x = scroll_scale(scroll_accum_x, &scroll_rem_x);
                    scroll_y = scroll_scale(scroll_accum_y, &scroll_rem_y);

                    scroll_accum_x = 0;
                    scroll_accum_y = 0;
                    last_scroll_send_ms = now_ms;
                }
                // else: scroll_x/scroll_y stay 0 for this iteration since
                // motion is accumulating, not being sent yet

                int Scroll_INTERVAL = CONFIG_TRACKPAD_SCROLL_INTERVAL;
                k_sleep(K_MSEC(Scroll_INTERVAL));
                x = 0;
                y = 0;
            } else {
                // fix9900: not in scroll mode this tick, drop any partial
                // accumulation so it can't leak into the next scroll gesture
                scroll_accum_x = 0;
                scroll_accum_y = 0;
                scroll_rem_x = 0;
                scroll_rem_y = 0;
                // The scaled value is worked out at full width and clamped
                // before it goes back into the 8-bit report field. It used
                // to be assigned straight to an int8_t, so a fast movement,
                // or a burst of sensor noise as a finger lifts, scaled past
                // 127 and wrapped round to the opposite sign: the pointer
                // jumped backwards.
                int scaled_x = x * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_HORIZONTAL / 100;
                int scaled_y = y * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_VERTICAL / 100;
                x = CLAMP(scaled_x, -127, 127);
                y = CLAMP(scaled_y, -127, 127);
            }
            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_movement_update(x, y);
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
            zmk_endpoints_send_mouse_report();
        } else if (strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_q10) == 0 ||
                   strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_q30) == 0 ||
                   strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_9981) == 0 ||
                   strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_9983) == 0) {
            int8_t x = xy_pos.val1;
            int8_t y = -1 * xy_pos.val2;
            // LOG_DBG("x value : %d , y value : %d\r\n", val->val1, val->val2);
            int8_t scroll_x = 0;
            int8_t scroll_y = 0;
            if (zmk_hid_indicators_get_current_profile() == 2 ||
                zmk_hid_indicators_get_current_profile() == 3 ||
                zmk_hid_indicators_get_current_profile() == 7 ||
                zmk_hid_indicators_get_current_profile() == 4) {
                if (abs(y) >= 128) {
                    scroll_x = -x / 24;
                    scroll_y = -y / 24;
                } else if (abs(y) >= 64 && abs(y) < 128) {
                    scroll_x = -x / 16;
                    scroll_y = -y / 16;
                } else if (abs(y) >= 32 && abs(y) < 64) {
                    scroll_x = -x / 12;
                    scroll_y = -y / 12;
                } else if (abs(y) >= 21 && abs(y) < 32) {
                    scroll_x = -x / 8;
                    scroll_y = -y / 8;
                } else if (abs(y) >= 3 && abs(y) < 20) {
                    scroll_x = -(x > 0) ? 1 : (x < 0) ? -1 : 0;
                    scroll_y = -((y > 0) ? 1 : (y < 0) ? -1 : 0);
                } else if (abs(y) >= 0 && abs(y) < 2) {
                    scroll_x = -(x > 0) ? 1 : (x < 0) ? -1 : 0;
                    ;
                    scroll_y = 0;
                }
                int Scroll_INTERVAL = CONFIG_TRACKPAD_SCROLL_INTERVAL;
                k_sleep(K_MSEC(Scroll_INTERVAL));
                x = 0;
                y = 0;
            } else {
                x = ((x < 127) ? x : (x - 256)) * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_HORIZONTAL /
                    100;
                y = ((y < 127) ? y : (y - 256)) * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_VERTICAL /
                    100;
            }
            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_movement_update(x, y);
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
            zmk_endpoints_send_mouse_report();
        } else if (strcmp(CONFIG_ZMK_KEYBOARD_NAME, target_q20) == 0) {
            int8_t x = -1 * xy_pos.val1;
            int8_t y = xy_pos.val2;
            // LOG_DBG("x value : %d , y value : %d\r\n", val->val1, val->val2);
            int8_t scroll_x = 0;
            int8_t scroll_y = 0;
            if (zmk_hid_indicators_get_current_profile() == 2 ||
                zmk_hid_indicators_get_current_profile() == 3 ||
                zmk_hid_indicators_get_current_profile() == 7 ||
                zmk_hid_indicators_get_current_profile() == 4) {
                if (abs(y) >= 128) {
                    scroll_x = -x / 24;
                    scroll_y = -y / 24;
                } else if (abs(y) >= 64 && abs(y) < 128) {
                    scroll_x = -x / 16;
                    scroll_y = -y / 16;
                } else if (abs(y) >= 32 && abs(y) < 64) {
                    scroll_x = -x / 12;
                    scroll_y = -y / 12;
                } else if (abs(y) >= 21 && abs(y) < 32) {
                    scroll_x = -x / 8;
                    scroll_y = -y / 8;
                } else if (abs(y) >= 3 && abs(y) < 20) {
                    scroll_x = -(x > 0) ? 1 : (x < 0) ? -1 : 0;
                    scroll_y = -((y > 0) ? 1 : (y < 0) ? -1 : 0);
                } else if (abs(y) >= 0 && abs(y) < 2) {
                    scroll_x = -(x > 0) ? 1 : (x < 0) ? -1 : 0;
                    ;
                    scroll_y = 0;
                }
                int Scroll_INTERVAL = CONFIG_TRACKPAD_SCROLL_INTERVAL;
                k_sleep(K_MSEC(Scroll_INTERVAL));
                x = 0;
                y = 0;
            } else {
                x = ((x < 127) ? x : (x - 256)) * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_HORIZONTAL /
                    100;
                y = ((y < 127) ? y : (y - 256)) * 1.5 * CONFIG_TRACKPAD_SPEEDMULTIPLIER_VERTICAL /
                    100;
            }
            zmk_hid_mouse_movement_set(0, 0);
            zmk_hid_mouse_movement_update(x, y);
            zmk_hid_mouse_scroll_set(0, 0);
            zmk_hid_mouse_scroll_update(scroll_x, scroll_y);
            zmk_endpoints_send_mouse_report();
        }
        int polling_ms = (1.0 / (float)CONFIG_INPUT_A320_POLLINGRATE) * 1000.0;
        k_sleep(K_MSEC(polling_ms));
    }
}
