/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_trackpad_light

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zmk/behavior.h>
#include <zmk/trackpad.h>
#include <dt-bindings/zmk/trackpad.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// The trackpad light from the keymap: on, off or toggle (saved), optional
// brightness cycling, and suspend/resume for the keyboard lock (not saved).
// Does nothing in a build without CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT.

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_trackpad_light_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT)
    switch (binding->param1) {
    case TPL_OFF:
        zmk_trackpad_light_set_enabled(false);
        break;
    case TPL_ON:
        zmk_trackpad_light_set_enabled(true);
        break;
    case TPL_TOG:
        zmk_trackpad_light_set_enabled(!zmk_trackpad_light_enabled());
        break;
    case TPL_CYCLE:
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_LIGHT_CYCLE)
    {
        uint8_t brightness = zmk_trackpad_light_get_brightness();
        brightness = brightness >= 100
                         ? 0
                         : MIN(brightness + CONFIG_ZMK_TRACKPAD_LIGHT_BRT_STEP, 100);
        zmk_trackpad_light_set_brightness(brightness);
        break;
    }
#else
        LOG_ERR("TPL_CYCLE requires CONFIG_ZMK_TRACKPAD_LIGHT_CYCLE");
        return -ENOTSUP;
#endif
    case TPL_SUSPEND:
        zmk_trackpad_light_set_suspended(true);
        break;
    case TPL_RESUME:
        zmk_trackpad_light_set_suspended(false);
        break;
    default:
        LOG_ERR("Unknown trackpad light command: %d", binding->param1);
        return -ENOTSUP;
    }
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_trackpad_light_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_trackpad_light_driver_api = {
    .binding_pressed = on_trackpad_light_pressed,
    .binding_released = on_trackpad_light_released,
};

static int behavior_trackpad_light_init(const struct device *dev) { return 0; }

#define TPLT_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_trackpad_light_init, NULL, NULL, NULL, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_trackpad_light_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TPLT_INST)

#endif
