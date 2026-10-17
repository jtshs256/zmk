/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_uconsole_lock_toggle

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>

#include <drivers/behavior.h>
#include <zmk/backlight.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/trackpad.h>
#include <zmk/fn.h>
#include <zmk/uconsole_state.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define UCONSOLE_LOCK_LAYER 5

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool fn_is_down(void) {
    return zmk_fn_is_down();
}

static int on_lock_kb_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    // This behavior is only bound to Fn-layer Esc positions and LOCK Esc.
    // Checking the momentary layer state prevents Esc alone from unlocking.
    if (!fn_is_down()) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (!zmk_keymap_layer_active(UCONSOLE_LOCK_LAYER)) {
        zmk_trackpad_set_enabled(false);

        int ret = zmk_backlight_suspend_fade();
        if (ret < 0) {
            LOG_ERR("uConsole: backlight suspend failed: %d", ret);
        }

        zmk_trackpad_light_set_suspended_fade(true);

        ret = zmk_keymap_layer_activate(UCONSOLE_LOCK_LAYER);
        if (ret < 0) {
            LOG_ERR("uConsole: LOCK layer activation failed: %d", ret);
            zmk_trackpad_set_enabled(true);
            ret = zmk_backlight_resume();
            if (ret < 0) {
                LOG_ERR("uConsole: backlight rollback failed: %d", ret);
            }
            zmk_trackpad_light_set_suspended(false);
        }

        return ZMK_BEHAVIOR_OPAQUE;
    }

#if IS_ENABLED(CONFIG_ZMK_UCONSOLE_FORCE_LOCK)
    if (zmk_uconsole_host_locked()) {
        LOG_DBG("uConsole: local unlock blocked by host force lock");
        return ZMK_BEHAVIOR_OPAQUE;
    }
#endif

    zmk_trackpad_set_enabled(true);

    int ret = zmk_backlight_resume();
    if (ret < 0) {
        LOG_ERR("uConsole: backlight resume failed: %d", ret);
    }

    zmk_trackpad_light_set_suspended(false);

    ret = zmk_keymap_layer_deactivate(UCONSOLE_LOCK_LAYER);
    if (ret < 0) {
        LOG_ERR("uConsole: LOCK layer deactivation failed: %d", ret);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_lock_kb_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_lock_kb_driver_api = {
    .binding_pressed = on_lock_kb_pressed,
    .binding_released = on_lock_kb_released,
};

static int behavior_lock_kb_init(const struct device *dev) { return 0; }

#define LOCK_KB_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_lock_kb_init, NULL, NULL, NULL, POST_KERNEL,               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_lock_kb_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LOCK_KB_INST)

#endif
