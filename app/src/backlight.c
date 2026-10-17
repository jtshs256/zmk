/*
 * Copyright (c) 2021 The ZMK Contributors
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

#include <zmk/activity.h>
#include <zmk/backlight.h>
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT)
#include <zmk/trackpad.h>
#endif
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_backlight),
             "CONFIG_ZMK_BACKLIGHT is enabled but no zmk,backlight chosen node found");

static const struct device *const backlight_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));

#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))

#define BACKLIGHT_NUM_LEDS (DT_NUM_CHILD(DT_CHOSEN(zmk_backlight)))

#define BRT_MAX 100

struct backlight_state {
    uint8_t brightness;
    bool on;
};

static struct backlight_state state = {.brightness = CONFIG_ZMK_BACKLIGHT_BRT_START,
                                       .on = IS_ENABLED(CONFIG_ZMK_BACKLIGHT_ON_START)};

// While suspended the LEDs are held off, whatever the on/off state says.
// The state itself is left alone (and so is never saved as "off"), which is
// what lets resume bring back exactly what was there before. Not persisted.
static bool suspended = false;

#define BACKLIGHT_SUSPEND_FADE_DURATION_MS 1000
#define BACKLIGHT_SUSPEND_FADE_INTERVAL_MS 20

static bool suspend_fade_active;
static uint8_t suspend_fade_start_brightness;
static uint8_t suspend_fade_brightness;
static int64_t suspend_fade_start_time;
static struct k_work_delayable suspend_fade_work;
static void backlight_suspend_fade_work_handler(struct k_work *work);

enum backlight_auto_off_flash_phase {
    BACKLIGHT_AUTO_OFF_FLASH_IDLE,
    BACKLIGHT_AUTO_OFF_FLASH_DARK,
    BACKLIGHT_AUTO_OFF_FLASH_LIT,
};

static enum backlight_auto_off_flash_phase auto_off_flash_phase;
static struct k_work_delayable auto_off_flash_work;
static void backlight_auto_off_flash_work_handler(struct k_work *work);

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
#define BACKLIGHT_IDLE_FADE_DURATION_MS 1000
#define BACKLIGHT_IDLE_FADE_INTERVAL_MS 20

static bool auto_off_idle;
static bool auto_off_idle_enabled = true;
static bool idle_prev_state;
static bool idle_fade_active;
static uint8_t idle_fade_start_brightness;
static uint8_t idle_fade_brightness;
static int64_t idle_fade_start_time;
static struct k_work_delayable idle_fade_work;
static void backlight_idle_fade_work_handler(struct k_work *work);
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
static bool auto_off_usb;
#endif
static void backlight_update_trackpad_auto_off(bool fade);
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE_TOGGLE)
static int backlight_auto_state(bool *prev_state, bool new_state, bool *auto_off_state,
                                bool fade);
#endif
#endif

static int zmk_backlight_update() {
    uint8_t brt = suspended ? 0 : zmk_backlight_get_brt();
    if (!suspended) {
        if (auto_off_flash_phase == BACKLIGHT_AUTO_OFF_FLASH_DARK) {
            brt = 0;
        } else if (auto_off_flash_phase == BACKLIGHT_AUTO_OFF_FLASH_LIT) {
            brt = MAX(state.brightness, CONFIG_ZMK_BACKLIGHT_BRT_STEP);
        }
    }
    LOG_DBG("Update backlight brightness: %d%%", brt);

    for (int i = 0; i < BACKLIGHT_NUM_LEDS; i++) {
        int rc = led_set_brightness(backlight_dev, i, brt);
        if (rc != 0) {
            LOG_ERR("Failed to update backlight LED %d: %d", i, rc);
            return rc;
        }
    }
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_SYNC_BACKLIGHT)
    zmk_trackpad_light_backlight_sync();
#endif
    return 0;
}

static void backlight_suspend_fade_work_handler(struct k_work *work) {
    if (!suspend_fade_active) {
        return;
    }

    int64_t elapsed = k_uptime_get() - suspend_fade_start_time;
    if (elapsed >= BACKLIGHT_SUSPEND_FADE_DURATION_MS) {
        suspend_fade_active = false;
        suspended = true;
        zmk_backlight_update();
        return;
    }

    uint32_t remaining = (uint32_t)(BACKLIGHT_SUSPEND_FADE_DURATION_MS - elapsed);
    suspend_fade_brightness =
        (uint8_t)((suspend_fade_start_brightness * remaining) / BACKLIGHT_SUSPEND_FADE_DURATION_MS);
    zmk_backlight_update();
    k_work_reschedule(&suspend_fade_work, K_MSEC(BACKLIGHT_SUSPEND_FADE_INTERVAL_MS));
}

static void backlight_suspend_fade_cancel(void) {
    if (!suspend_fade_active) {
        return;
    }

    suspend_fade_active = false;
    k_work_cancel_delayable(&suspend_fade_work);
}

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
static void backlight_idle_fade_work_handler(struct k_work *work) {
    if (!idle_fade_active) {
        return;
    }

    int64_t elapsed = k_uptime_get() - idle_fade_start_time;
    if (elapsed >= BACKLIGHT_IDLE_FADE_DURATION_MS) {
        idle_fade_active = false;
        idle_fade_brightness = 0;
        zmk_backlight_update();
        return;
    }

    uint32_t remaining = (uint32_t)(BACKLIGHT_IDLE_FADE_DURATION_MS - elapsed);
    idle_fade_brightness =
        (uint8_t)((idle_fade_start_brightness * remaining) / BACKLIGHT_IDLE_FADE_DURATION_MS);
    zmk_backlight_update();
    k_work_reschedule(&idle_fade_work, K_MSEC(BACKLIGHT_IDLE_FADE_INTERVAL_MS));
}

static void backlight_idle_fade_start(void) {
    idle_fade_start_brightness = zmk_backlight_get_brt();
    if (suspended || idle_fade_start_brightness == 0) {
        return;
    }

    idle_fade_brightness = idle_fade_start_brightness;
    idle_fade_start_time = k_uptime_get();
    idle_fade_active = true;
    k_work_reschedule(&idle_fade_work, K_MSEC(BACKLIGHT_IDLE_FADE_INTERVAL_MS));
}

static void backlight_idle_fade_cancel(void) {
    idle_fade_active = false;
    k_work_cancel_delayable(&idle_fade_work);
}
#endif

static void backlight_auto_off_flash_work_handler(struct k_work *work) {
    if (auto_off_flash_phase == BACKLIGHT_AUTO_OFF_FLASH_DARK) {
        auto_off_flash_phase = BACKLIGHT_AUTO_OFF_FLASH_LIT;
        zmk_backlight_update();
        k_work_reschedule(&auto_off_flash_work, K_MSEC(100));
    } else if (auto_off_flash_phase == BACKLIGHT_AUTO_OFF_FLASH_LIT) {
        auto_off_flash_phase = BACKLIGHT_AUTO_OFF_FLASH_IDLE;
        zmk_backlight_update();
    }
}

static void backlight_auto_off_flash_start(void) {
    if (suspended) {
        return;
    }

    auto_off_flash_phase = BACKLIGHT_AUTO_OFF_FLASH_DARK;
    zmk_backlight_update();
    k_work_reschedule(&auto_off_flash_work, K_MSEC(60));
}

static void backlight_auto_off_flash_cancel(void) {
    k_work_cancel_delayable(&auto_off_flash_work);
    if (auto_off_flash_phase != BACKLIGHT_AUTO_OFF_FLASH_IDLE) {
        auto_off_flash_phase = BACKLIGHT_AUTO_OFF_FLASH_IDLE;
        zmk_backlight_update();
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int backlight_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg, void *param) {
    const char *next;
    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &state, sizeof(state));
        return MIN(rc, 0);
    }
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE_TOGGLE)
    if (settings_name_steq(name, "auto_off_idle", &next) && !next) {
        if (len != sizeof(auto_off_idle_enabled)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &auto_off_idle_enabled, sizeof(auto_off_idle_enabled));
        return MIN(rc, 0);
    }
#endif
    return -ENOENT;
}

static void backlight_save_work_handler(struct k_work *work) {
    settings_save_one("backlight/state", &state, sizeof(state));
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE_TOGGLE)
    settings_save_one("backlight/auto_off_idle", &auto_off_idle_enabled,
                      sizeof(auto_off_idle_enabled));
#endif
}

static struct k_work_delayable backlight_save_work;

static int backlight_schedule_save(void) {
    int ret = k_work_reschedule(&backlight_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
}
#endif

static int zmk_backlight_init(void) {
    if (!device_is_ready(backlight_dev)) {
        LOG_ERR("Backlight device \"%s\" is not ready", backlight_dev->name);
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    int rc = settings_load_subtree_direct("backlight", backlight_settings_load_cb, NULL);
    if (rc != 0) {
        LOG_ERR("Failed to load backlight settings: %d", rc);
    }
    k_work_init_delayable(&backlight_save_work, backlight_save_work_handler);
#endif
    k_work_init_delayable(&auto_off_flash_work, backlight_auto_off_flash_work_handler);
    k_work_init_delayable(&suspend_fade_work, backlight_suspend_fade_work_handler);
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    k_work_init_delayable(&idle_fade_work, backlight_idle_fade_work_handler);
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
    auto_off_usb = !state.on;
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
    backlight_update_trackpad_auto_off(false);
#endif
    return zmk_backlight_update();
}

static int zmk_backlight_update_and_save() {
    int rc = zmk_backlight_update();
    if (rc != 0) {
        return rc;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    return backlight_schedule_save();
#else
    return 0;
#endif
}

int zmk_backlight_on() {
    backlight_suspend_fade_cancel();
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    backlight_idle_fade_cancel();
#endif
    state.brightness = MAX(state.brightness, CONFIG_ZMK_BACKLIGHT_BRT_STEP);
    state.on = true;
    return zmk_backlight_update_and_save();
}

int zmk_backlight_off() {
    backlight_suspend_fade_cancel();
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    backlight_idle_fade_cancel();
#endif
    state.on = false;
    return zmk_backlight_update_and_save();
}

int zmk_backlight_toggle() { return state.on ? zmk_backlight_off() : zmk_backlight_on(); }

void zmk_backlight_flash(void) { backlight_auto_off_flash_start(); }

int zmk_backlight_suspend() {
    backlight_suspend_fade_cancel();
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    backlight_idle_fade_cancel();
#endif
    suspended = true;
    backlight_auto_off_flash_cancel();
    return zmk_backlight_update();
}

int zmk_backlight_suspend_fade(void) {
    if (suspended || suspend_fade_active) {
        return 0;
    }

    backlight_auto_off_flash_cancel();

    uint8_t brightness = zmk_backlight_get_brt();
    if (brightness == 0) {
        return zmk_backlight_suspend();
    }

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    backlight_idle_fade_cancel();
#endif
    suspend_fade_start_brightness = brightness;
    suspend_fade_brightness = brightness;
    suspend_fade_start_time = k_uptime_get();
    suspend_fade_active = true;
    k_work_reschedule(&suspend_fade_work, K_MSEC(BACKLIGHT_SUSPEND_FADE_INTERVAL_MS));
    return zmk_backlight_update();
}

int zmk_backlight_resume() {
    backlight_suspend_fade_cancel();
    suspended = false;
    return zmk_backlight_update();
}

bool zmk_backlight_is_on() { return state.on; }

int zmk_backlight_set_brt(uint8_t brightness) {
    backlight_suspend_fade_cancel();
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    backlight_idle_fade_cancel();
#endif
    state.brightness = MIN(brightness, BRT_MAX);
    state.on = (state.brightness > 0);
    return zmk_backlight_update_and_save();
}

uint8_t zmk_backlight_get_brt() {
    if (suspend_fade_active) {
        return suspend_fade_brightness;
    }
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    if (idle_fade_active) {
        return idle_fade_brightness;
    }
#endif
    return state.on ? state.brightness : 0;
}

uint8_t zmk_backlight_calc_brt(int direction) {
    int brt = state.brightness + (direction * CONFIG_ZMK_BACKLIGHT_BRT_STEP);
    return CLAMP(brt, 0, BRT_MAX);
}

uint8_t zmk_backlight_calc_brt_cycle() {
    if (state.brightness == BRT_MAX) {
        return 0;
    } else {
        return zmk_backlight_calc_brt(1);
    }
}

int zmk_backlight_toggle_auto_off_idle() {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE_TOGGLE)
    bool currently_active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
    bool enable_auto_off = !auto_off_idle_enabled;
    auto_off_idle_enabled = enable_auto_off;

    // Apply a changed setting immediately if the keyboard is already idle.
    int rc = 0;
    if (!currently_active) {
        rc = backlight_auto_state(&idle_prev_state, !enable_auto_off, &auto_off_idle, false);
    } else {
        backlight_update_trackpad_auto_off(false);
    }

    if (rc != 0) {
        return rc;
    }
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE_TOGGLE)
    if (enable_auto_off) {
        backlight_auto_off_flash_start();
    } else {
        backlight_auto_off_flash_cancel();
    }
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
    return backlight_schedule_save();
#else
    return 0;
#endif
#else
    return -ENOTSUP;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
static void backlight_update_trackpad_auto_off(bool fade) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT)
    bool auto_off = false;
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    auto_off = auto_off || auto_off_idle;
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
    auto_off = auto_off || auto_off_usb;
#endif
    zmk_trackpad_light_set_auto_off(auto_off, fade);
#else
    (void)fade;
#endif
}

static int backlight_auto_state(bool *prev_state, bool new_state, bool *auto_off_state,
                                bool fade) {
    *auto_off_state = !new_state;
    backlight_update_trackpad_auto_off(fade);

    if (state.on == new_state) {
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
        if (!new_state && idle_fade_active && !fade) {
            backlight_idle_fade_cancel();
            return zmk_backlight_update();
        }
#endif
        return 0;
    }
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    if (new_state) {
        backlight_idle_fade_cancel();
    } else if (fade) {
        backlight_idle_fade_start();
    }
#endif
    state.on = new_state && *prev_state;
    *prev_state = !new_state;
    return zmk_backlight_update();
}

static int backlight_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        // Use the state carried by this event. Trackpad polling can notify
        // activity repeatedly while older activity events are still waiting
        // to be handled; reading the global state here can make an IDLE event
        // look ACTIVE and lose the off/on transitions.
        bool active = as_zmk_activity_state_changed(eh)->state == ZMK_ACTIVITY_ACTIVE;
        if (!auto_off_idle_enabled) {
            // Keep the trackpad policy in sync while the runtime option is disabled.
            auto_off_idle = false;
            backlight_update_trackpad_auto_off(false);
            return 0;
        }
        return backlight_auto_state(&idle_prev_state, active, &auto_off_idle, !active);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        static bool prev_state = false;
        return backlight_auto_state(&prev_state, zmk_usb_is_powered(), &auto_off_usb, false);
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(backlight, backlight_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(backlight, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(backlight, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_backlight_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
