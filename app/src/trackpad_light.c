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

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
#include <zmk/backlight.h>
#endif
#include <zmk/led_fade.h>
#include <zmk/trackpad.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// The trackpad light can follow scroll mode, and can also be controlled by
// independent brightness, backlight-sync, and suspend options.
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
             "Trackpad light control is enabled but no zmk_indicator_capslock chosen "
             "node found");

static const struct device *const light_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_indicator_capslock));

static uint8_t light_brightness = 100;
static bool light_suspended;
static bool light_auto_off;
static bool light_scrolling;
static bool light_initialized;
static bool light_backlight_flash_active;
static uint8_t light_get_brightness(void);
static void light_refresh(void);

static bool light_fade_active;
static bool light_suspend_after_fade;
static uint8_t light_fade_start_brightness;
static uint8_t light_fade_target_brightness;
static uint32_t light_fade_duration_ms;
static int64_t light_fade_start_time;
static struct k_work_delayable light_fade_work;
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
static bool light_output_initialized;
static bool light_resume_fade_pending;
static uint8_t light_last_output_brightness;
static int64_t light_fade_skip_until;
#endif
static void light_fade_cancel(void);

// Convert logical brightness (0 = dark, 100 = lit) to the board's LED level.
static int light_level_from_brightness(int brightness) {
    return IS_ENABLED(CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT) ? brightness : 100 - brightness;
}

static uint8_t light_fade_current_brightness(void) {
    int64_t elapsed = k_uptime_get() - light_fade_start_time;
    if (elapsed >= light_fade_duration_ms) {
        return light_fade_target_brightness;
    }

    int32_t delta = (int32_t)light_fade_target_brightness - light_fade_start_brightness;
    return (uint8_t)(light_fade_start_brightness +
                     (delta * elapsed) / light_fade_duration_ms);
}

static void light_fade_start(uint8_t start_brightness, uint8_t target_brightness,
                             uint32_t duration_ms, bool suspend_after_fade) {
    light_fade_cancel();
    light_fade_start_brightness = start_brightness;
    light_fade_target_brightness = target_brightness;
    light_fade_duration_ms = duration_ms;
    light_fade_start_time = k_uptime_get();
    light_fade_active = true;
    light_suspend_after_fade = suspend_after_fade;
    k_work_reschedule(&light_fade_work, K_MSEC(ZMK_LED_FADE_INTERVAL_MS));
}

static void light_fade_work_handler(struct k_work *work) {
    if (!light_fade_active) {
        return;
    }

    if (light_suspended) {
        light_fade_active = false;
        light_suspend_after_fade = false;
        return;
    }

    int64_t elapsed = k_uptime_get() - light_fade_start_time;
    if (elapsed >= light_fade_duration_ms) {
        bool suspend_after_fade = light_suspend_after_fade;
        light_fade_active = false;
        light_suspend_after_fade = false;
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
        light_last_output_brightness = light_fade_target_brightness;
#endif
        if (suspend_after_fade) {
            zmk_trackpad_light_set_suspended(true);
        } else {
            light_refresh();
        }
        return;
    }

    uint8_t brightness = light_fade_current_brightness();
    if (device_is_ready(light_dev)) {
        int rc = led_set_brightness(light_dev, 0, light_level_from_brightness(brightness));
        if (rc != 0) {
            LOG_ERR("Failed to fade the trackpad light: %d", rc);
        } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
            light_last_output_brightness = brightness;
#endif
        }
    }

    k_work_reschedule(&light_fade_work, K_MSEC(ZMK_LED_FADE_INTERVAL_MS));
}

static void light_fade_cancel(void) {
    if (light_fade_active) {
        light_fade_active = false;
        k_work_cancel_delayable(&light_fade_work);
    }

    light_suspend_after_fade = false;
}

// Optional breathing effect while suspended/locked. Enable with:
// CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING=y
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
// One full dark -> light -> dark cycle is approximately 3.5 seconds.
#define BREATHE_STEP 2
#define BREATHE_INTERVAL_MS 35
#endif

static uint8_t light_get_brightness(void) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    return zmk_backlight_get_brt();
#else
    return light_brightness;
#endif
}

#define LIGHT_SETTINGS_VERSION 1

struct light_settings {
    uint8_t version;
    uint8_t brightness;
};

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
static struct k_work_delayable breathe_work;
static int breathe_brightness;
static int breathe_direction = 1;

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

    if (!light_suspended) {
        return;
    }

    if (device_is_ready(light_dev)) {
        int rc =
            led_set_brightness(light_dev, 0, light_level_from_brightness(breathe_brightness));
        if (rc != 0) {
            LOG_ERR("Failed to set the trackpad breathing light: %d", rc);
        } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
            light_last_output_brightness = breathe_brightness;
            light_output_initialized = true;
#endif
        }
    }

    if (light_suspended) {
        k_work_reschedule(&breathe_work, K_MSEC(BREATHE_INTERVAL_MS));
    }
}
#endif

static void light_refresh(void) {
    if (!device_is_ready(light_dev)) {
        return;
    }

    if (light_fade_active || light_backlight_flash_active) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    // While suspended, breathe_work owns the LED, including when the regular
    // trackpad light is suppressed by keyboard idle auto-off.
    if (light_suspended) {
        return;
    }
    const uint8_t brightness = (!light_auto_off && !light_scrolling) ? light_get_brightness() : 0;
#else
    // Original behavior: suspension holds the light dark.
    const uint8_t brightness = (!light_suspended && !light_auto_off && !light_scrolling)
                                   ? light_get_brightness()
                                   : 0;
#endif
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    int64_t now = k_uptime_get();
    bool skip_wake_fade = light_fade_skip_until > now;
    if (light_fade_skip_until != 0 && !skip_wake_fade) {
        light_fade_skip_until = 0;
    }

    if (light_resume_fade_pending && brightness > 0) {
        light_resume_fade_pending = false;
        if (!skip_wake_fade) {
            uint8_t start_brightness = light_last_output_brightness;
            light_fade_start(start_brightness, brightness, ZMK_LED_WAKE_FADE_DURATION_MS, false);
            return;
        }
    }

    if (light_resume_fade_pending && light_get_brightness() == 0) {
        light_resume_fade_pending = false;
    }

    if (light_output_initialized && light_last_output_brightness == 0 && brightness > 0) {
        if (!skip_wake_fade) {
            light_fade_start(0, brightness, ZMK_LED_WAKE_FADE_DURATION_MS, false);
            return;
        }
    }

    if (skip_wake_fade && brightness > 0) {
        light_fade_skip_until = 0;
    }
#endif
    const uint8_t level = light_level_from_brightness(brightness);
    int rc = led_set_brightness(light_dev, 0, level);
    if (rc != 0) {
        LOG_ERR("Failed to set the trackpad light: %d", rc);
    } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
        light_last_output_brightness = brightness;
        light_output_initialized = true;
#endif
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int light_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg, void *param) {
    const char *next;
    if (settings_name_steq(name, "light", &next) && !next) {
        if (len == sizeof(uint8_t)) {
            // Older firmware stored the on/off state as a single byte.
            uint8_t saved;
            int rc = read_cb(cb_arg, &saved, sizeof(saved));
            if (rc >= 0) {
                light_brightness = saved ? 100 : 0;
            }
            return MIN(rc, 0);
        }

        struct light_settings saved;
        if (len != sizeof(saved)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &saved, sizeof(saved));
        if (rc >= 0 && saved.version == LIGHT_SETTINGS_VERSION) {
            light_brightness = MIN(saved.brightness, 100);
        }
        return MIN(rc, 0);
    }
    return -ENOENT;
}

static void light_save_work_handler(struct k_work *work) {
    struct light_settings saved = {
        .version = LIGHT_SETTINGS_VERSION,
        .brightness = light_brightness,
    };
    settings_save_one("trackpad/light", &saved, sizeof(saved));
}

static struct k_work_delayable light_save_work;
#endif

void zmk_trackpad_scroll_light_update(bool scrolling) {
    light_scrolling = scrolling;
    if (scrolling) {
        light_fade_cancel();
    }
    light_refresh();
}

void zmk_trackpad_light_backlight_sync(void) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    if (light_initialized) {
        light_refresh();
    }
#endif
}

void zmk_trackpad_light_backlight_flash(bool flashing, uint8_t backlight_brightness) {
    if (!flashing) {
        if (light_backlight_flash_active) {
            light_backlight_flash_active = false;
            light_refresh();
        }
        return;
    }

    if (light_suspended || light_suspend_after_fade || !device_is_ready(light_dev)) {
        return;
    }

    light_fade_cancel();
    light_backlight_flash_active = true;
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    const uint8_t brightness = MIN(backlight_brightness, 100);
#else
    const uint8_t brightness = light_get_brightness();
#endif
    const uint8_t logical_brightness =
        backlight_brightness == 0 || brightness == 0 ? 0 : brightness;
    const uint8_t level = light_level_from_brightness(logical_brightness);
    int rc = led_set_brightness(light_dev, 0, level);
    if (rc != 0) {
        LOG_ERR("Failed to flash the trackpad light: %d", rc);
    } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
        light_last_output_brightness = brightness;
        light_output_initialized = true;
#endif
    }
}

bool zmk_trackpad_light_enabled(void) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    return zmk_backlight_is_on();
#else
    return light_brightness > 0;
#endif
}

void zmk_trackpad_light_set_enabled(bool on) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    light_fade_cancel();
    if (on) {
        zmk_backlight_on();
    } else {
        zmk_backlight_off();
    }
#else
    zmk_trackpad_light_set_brightness(on ? 100 : 0);
#endif
}

uint8_t zmk_trackpad_light_get_brightness(void) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    return zmk_backlight_get_brt();
#else
    return light_brightness;
#endif
}

void zmk_trackpad_light_set_brightness(uint8_t brightness) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    light_fade_cancel();
    zmk_backlight_set_brt(MIN(brightness, 100));
#else
    brightness = MIN(brightness, 100);
    if (light_brightness == brightness) {
        return;
    }

    uint8_t current_brightness =
        light_fade_active ? light_fade_current_brightness() : light_last_output_brightness;
    light_fade_cancel();
    light_brightness = brightness;
    if (brightness == 0 && current_brightness > 0 && !light_suspended) {
        light_fade_start(current_brightness, 0, ZMK_LED_TRANSITION_FADE_DURATION_MS, false);
    } else {
        light_refresh();
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_reschedule(&light_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
#endif
#endif
}

void zmk_trackpad_light_set_suspended(bool suspended) {
    bool was_suspended = light_suspended;
    if (!suspended && light_suspend_after_fade) {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
        light_resume_fade_pending = true;
#endif
        light_fade_cancel();
        light_refresh();
    }

    if (was_suspended == suspended) {
        if (!suspended) {
            light_refresh();
        }
        return;
    }

    if (suspended) {
        light_backlight_flash_active = false;
        light_fade_cancel();
    } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
        light_resume_fade_pending = true;
#endif
    }

    light_suspended = suspended;

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    if (suspended) {
        // Begin at fully dark, then fade toward fully lit.
        breathe_brightness = 0;
        breathe_direction = 1;

        if (device_is_ready(light_dev)) {
            int rc = led_set_brightness(light_dev, 0, light_level_from_brightness(0));
            if (rc != 0) {
                LOG_ERR("Failed to start the trackpad breathing light: %d", rc);
            } else {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
                light_last_output_brightness = 0;
                light_output_initialized = true;
#endif
            }
        }

        k_work_reschedule(&breathe_work, K_MSEC(BREATHE_INTERVAL_MS));
    } else {
        k_work_cancel_delayable(&breathe_work);
        light_refresh();
    }
#else
    light_refresh();
#endif
}

void zmk_trackpad_light_set_suspended_fade(bool suspended, uint32_t duration_ms) {
    if (!suspended) {
        zmk_trackpad_light_set_suspended(false);
        return;
    }

    if (light_suspended) {
        return;
    }

    uint8_t brightness = 0;
    if (light_fade_active) {
        brightness = light_fade_current_brightness();
    } else if (!light_auto_off && !light_scrolling) {
        brightness = light_get_brightness();
    }

    if (brightness == 0 || duration_ms == 0) {
        zmk_trackpad_light_set_suspended(true);
        return;
    }

    light_fade_start(brightness, 0, duration_ms, true);
}

void zmk_trackpad_light_skip_fade_on_activity(void) {
#if !IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    if (!light_fade_active || light_suspend_after_fade) {
        return;
    }

    light_fade_skip_until = k_uptime_get() + ZMK_LED_FADE_SKIP_WINDOW_MS;
    light_fade_cancel();
    light_refresh();
#endif
}

void zmk_trackpad_light_set_auto_off(bool auto_off, bool fade) {
    if (light_auto_off == auto_off) {
        if (auto_off && light_fade_active && !light_suspend_after_fade && !fade) {
            light_fade_cancel();
            light_refresh();
        }
        return;
    }

    bool was_auto_off = light_auto_off;
    light_auto_off = auto_off;

    if (auto_off && fade && !was_auto_off && !light_suspended && !light_scrolling) {
        uint8_t brightness = light_fade_active ? light_fade_current_brightness()
                                               : light_get_brightness();
        if (brightness > 0) {
            light_fade_start(brightness, 0, ZMK_LED_FADE_DURATION_MS, false);
            return;
        }
    }

    if (!auto_off && !light_suspend_after_fade) {
        light_fade_cancel();
    }

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SUSPEND_BREATHING)
    // Auto-off controls the normal trackpad light. A suspended light uses
    // the breathing effect instead, so changing auto-off must not stop it.
    if (light_suspended && !auto_off) {
        breathe_brightness = 0;
        breathe_direction = 1;
        k_work_reschedule(&breathe_work, K_MSEC(BREATHE_INTERVAL_MS));
    }
#endif

    light_refresh();
}

static int zmk_trackpad_scroll_light_init(void) {
    k_work_init_delayable(&light_fade_work, light_fade_work_handler);
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
    light_initialized = true;
    light_refresh();
    return 0;
}

SYS_INIT(zmk_trackpad_scroll_light_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
