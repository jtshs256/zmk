/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/trackpad.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// The trackpad light follows scroll mode.
//
// On the bb9900 the light under the trackpad is on the pin the board template
// calls the CapsLock indicator (the zmk_indicator_capslock chosen node). The
// vendor firmware drove that pin to 100% while CapsLock was on, and CapsLock
// was also what put its trackpad into scroll mode, so the light changed with
// scroll mode as a side effect. fix9900 switched the CapsLock indicator off,
// which left the pin undriven.
//
// This drives the same pin from the real trackpad state. On top of that the
// light can be switched off from the keymap (zmk,behavior-trackpad-light,
// saved). While the keyboard lock suspends the trackpad light, the LED uses a
// breathing effect. Resume stops the effect and restores the user's saved
// on/off choice. Whether 100% means lit or dark depends on how the board wires
// the light, so CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT swaps the two levels.
//
// It cannot be combined with CONFIG_ZMK_INDICATOR_CAPSLOCK, which polls and
// would overwrite the pin every 50 ms.

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_indicator_capslock),
             "CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT is enabled but no zmk_indicator_capslock chosen "
             "node found");

static const struct device *const light_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_indicator_capslock));

#define LEVEL_LIT (IS_ENABLED(CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT) ? 100 : 0)
#define LEVEL_DARK (IS_ENABLED(CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT) ? 0 : 100)

// Optional breathing effect while suspended/locked. Enable with:
// CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING=y
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
// One full dark -> light -> dark cycle is approximately 2.5 seconds.
#define BREATHE_STEP 2
#define BREATHE_INTERVAL_MS 25
#endif

static bool light_enabled = true;
static bool light_suspended;
static bool light_scrolling;

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
static struct k_work_delayable breathe_work;
static int breathe_brightness;
static int breathe_direction = 1;

// Convert a logical brightness where 0 is dark and 100 is fully lit into the
// physical level expected by the board wiring.
static int light_level_from_brightness(int brightness) {
    return IS_ENABLED(CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT) ? 100 - brightness : brightness;
}

static void breathe_work_handler(struct k_work *work) {
    if (!light_suspended) {
        return;
    }

    breathe_brightness += breathe_direction * BREATHE_STEP;

    if (breathe_brightness >= 100) {
        breathe_brightness = 100;
        breathe_direction = -1;
    } else if (breathe_brightness <= 0) {
        breathe_brightness = 0;
        breathe_direction = 1;
    }

    if (device_is_ready(light_dev)) {
        int rc =
            led_set_brightness(light_dev, 0, light_level_from_brightness(breathe_brightness));
        if (rc != 0) {
            LOG_ERR("Failed to set the trackpad breathing light: %d", rc);
        }
    }

    k_work_reschedule(&breathe_work, K_MSEC(BREATHE_INTERVAL_MS));
}
#endif

static void light_refresh(void) {
    if (!device_is_ready(light_dev)) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    // While suspended, breathe_work owns the LED. Scroll or saved-state
    // updates must not overwrite the current breathing brightness.
    if (light_suspended) {
        return;
    }
    const bool lit = light_enabled && !light_scrolling;
#else
    // Original behavior: suspension holds the light dark.
    const bool lit = light_enabled && !light_suspended && !light_scrolling;
#endif
    int rc = led_set_brightness(light_dev, 0, lit ? LEVEL_LIT : LEVEL_DARK);
    if (rc != 0) {
        LOG_ERR("Failed to set the trackpad light: %d", rc);
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int light_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg, void *param) {
    const char *next;
    if (settings_name_steq(name, "light", &next) && !next) {
        uint8_t saved;
        if (len != sizeof(saved)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &saved, sizeof(saved));
        if (rc >= 0) {
            light_enabled = saved != 0;
        }
        return MIN(rc, 0);
    }
    return -ENOENT;
}

static void light_save_work_handler(struct k_work *work) {
    uint8_t saved = light_enabled ? 1 : 0;
    settings_save_one("trackpad/light", &saved, sizeof(saved));
}

static struct k_work_delayable light_save_work;
#endif

void zmk_trackpad_scroll_light_update(bool scrolling) {
    light_scrolling = scrolling;
    light_refresh();
}

bool zmk_trackpad_light_enabled(void) { return light_enabled; }

void zmk_trackpad_light_set_enabled(bool on) {
    if (light_enabled == on) {
        return;
    }
    light_enabled = on;
    light_refresh();
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_reschedule(&light_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif
}

void zmk_trackpad_light_set_suspended(bool suspended) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    // Avoid restarting the breathing animation if the state did not change.
    if (light_suspended == suspended) {
        return;
    }

    light_suspended = suspended;

    if (suspended) {
        // Begin at fully dark, then fade toward fully lit.
        breathe_brightness = 0;
        breathe_direction = 1;

        if (device_is_ready(light_dev)) {
            int rc = led_set_brightness(light_dev, 0, light_level_from_brightness(0));
            if (rc != 0) {
                LOG_ERR("Failed to start the trackpad breathing light: %d", rc);
            }
        }

        k_work_reschedule(&breathe_work, K_MSEC(BREATHE_INTERVAL_MS));
    } else {
        k_work_cancel_delayable(&breathe_work);
        light_refresh();
    }
#else
    light_suspended = suspended;
    light_refresh();
#endif
}

static int zmk_trackpad_scroll_light_init(void) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    k_work_init_delayable(&breathe_work, breathe_work_handler);
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    int rc = settings_load_subtree_direct("trackpad", light_settings_load_cb, NULL);
    if (rc != 0) {
        LOG_ERR("Failed to load trackpad light settings: %d", rc);
    }
    k_work_init_delayable(&light_save_work, light_save_work_handler);
#endif
    light_refresh();
    return 0;
}

SYS_INIT(zmk_trackpad_scroll_light_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
