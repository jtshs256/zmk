/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_uconsole_unlock

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/backlight.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/trackpad.h>
#include <zmk/uconsole_state.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define UCONSOLE_LOCK_LAYER 5

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_uconsole_unlock_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
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

static int on_uconsole_unlock_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_uconsole_unlock_driver_api = {
    .binding_pressed = on_uconsole_unlock_pressed,
    .binding_released = on_uconsole_unlock_released,
};

static int behavior_uconsole_unlock_init(const struct device *dev) { return 0; }

#define UCONSOLE_UNLOCK_INST(n)                                                                    \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_uconsole_unlock_init, NULL, NULL, NULL, POST_KERNEL,       \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_uconsole_unlock_driver_api);

DT_INST_FOREACH_STATUS_OKAY(UCONSOLE_UNLOCK_INST)

#endif
