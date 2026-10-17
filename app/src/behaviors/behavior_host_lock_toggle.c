/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_host_lock_toggle

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/backlight.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/trackpad.h>
#include <zmk/fn.h>
#include <zmk/led_fade.h>
#include <zmk/host_lock.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

int zmk_host_lock_force_unlock(void) {
    int ret = zmk_keymap_layer_deactivate(ZMK_HOST_LOCK_LAYER);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_keymap_layer_deactivate(ZMK_HOST_UNLOCK_LEFT_LAYER);
    if (ret < 0) {
        return ret;
    }

    return zmk_keymap_layer_deactivate(ZMK_HOST_UNLOCK_RIGHT_LAYER);
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_host_lock_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    // This behavior is only bound to Fn-layer Esc positions and LOCK Esc.
    // Checking the momentary layer state prevents Esc alone from unlocking.
    if (!zmk_fn_is_down()) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (!zmk_keymap_layer_active(ZMK_HOST_LOCK_LAYER)) {
        zmk_trackpad_set_enabled(false);

        int ret = zmk_backlight_suspend_fade(ZMK_MANUAL_LOCK_FADE_DURATION_MS);
        if (ret < 0) {
            LOG_ERR("Host lock: backlight suspend failed: %d", ret);
        }

        zmk_trackpad_light_set_suspended_fade(true, ZMK_MANUAL_LOCK_FADE_DURATION_MS);

        ret = zmk_keymap_layer_activate(ZMK_HOST_LOCK_LAYER);
        if (ret < 0) {
            LOG_ERR("Host lock: LOCK layer activation failed: %d", ret);
            zmk_trackpad_set_enabled(true);
            ret = zmk_backlight_resume();
            if (ret < 0) {
                LOG_ERR("Host lock: backlight rollback failed: %d", ret);
            }
            zmk_trackpad_light_set_suspended(false);
        }

        return ZMK_BEHAVIOR_OPAQUE;
    }

#if IS_ENABLED(CONFIG_ZMK_HOST_FORCE_LOCK)
    if (zmk_host_lock_active()) {
        LOG_DBG("Host lock: local unlock blocked by host force lock");
        return ZMK_BEHAVIOR_OPAQUE;
    }
#endif

    zmk_trackpad_set_enabled(true);

    int ret = zmk_backlight_resume();
    if (ret < 0) {
        LOG_ERR("Host lock: backlight resume failed: %d", ret);
    }

    zmk_trackpad_light_set_suspended(false);

    ret = zmk_keymap_layer_deactivate(ZMK_HOST_LOCK_LAYER);
    if (ret < 0) {
        LOG_ERR("Host lock: LOCK layer deactivation failed: %d", ret);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_host_lock_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_host_lock_driver_api = {
    .binding_pressed = on_host_lock_pressed,
    .binding_released = on_host_lock_released,
};

static int behavior_host_lock_init(const struct device *dev) { return 0; }

#define HOST_LOCK_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_host_lock_init, NULL, NULL, NULL, POST_KERNEL,             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_host_lock_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HOST_LOCK_INST)

#endif
