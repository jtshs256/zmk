/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_fn

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/backlight.h>
#include <zmk/keymap.h>
#include <zmk/fn.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LOCK_LAYER 5
#define SYM_LAYER 1
#define UPPER_LAYER 2
#define UNLOCK_LEFT_LAYER 6
#define UNLOCK_RIGHT_LAYER 7
#define CONFIG_LAYER 8

enum fn_action {
    FN_LEFT = 0,
    FN_RIGHT = 1,
    FN_LOCK_SYM = 2,
};

enum fn_layer_target {
    FN_TARGET_NONE,
    FN_TARGET_SYM,
    FN_TARGET_UPPER,
    FN_TARGET_CONFIG,
    FN_TARGET_UNLOCK_LEFT,
    FN_TARGET_UNLOCK_RIGHT,
};

static bool left_fn_down;
static bool right_fn_down;
static bool sym_fn_locked;
static bool sym_lock_key_consumed;
static enum fn_layer_target left_fn_target;
static enum fn_layer_target right_fn_target;

bool zmk_fn_is_down(void) { return left_fn_down || right_fn_down; }

static uint8_t target_layer(enum fn_layer_target target) {
    switch (target) {
    case FN_TARGET_SYM:
        return SYM_LAYER;
    case FN_TARGET_UPPER:
        return UPPER_LAYER;
    case FN_TARGET_CONFIG:
        return CONFIG_LAYER;
    case FN_TARGET_UNLOCK_LEFT:
        return UNLOCK_LEFT_LAYER;
    case FN_TARGET_UNLOCK_RIGHT:
        return UNLOCK_RIGHT_LAYER;
    default:
        return 0;
    }
}

static int activate_target(enum fn_layer_target target) {
    uint8_t layer = target_layer(target);
    return layer == 0 ? -EINVAL : zmk_keymap_layer_activate(layer);
}

static int deactivate_target(enum fn_layer_target target) {
    uint8_t layer = target_layer(target);
    return layer == 0 ? -EINVAL : zmk_keymap_layer_deactivate(layer);
}

static int on_fn_left_pressed(struct zmk_behavior_binding_event event) {
    left_fn_down = true;
    left_fn_target = zmk_keymap_layer_active(LOCK_LAYER)
                         ? FN_TARGET_UNLOCK_LEFT
                         : FN_TARGET_SYM;

    int ret = activate_target(left_fn_target);
    if (ret < 0) {
        LOG_ERR("Failed to activate Left Fn layer: %d", ret);
        left_fn_down = false;
        left_fn_target = FN_TARGET_NONE;
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fn_left_released(struct zmk_behavior_binding_event event) {
    left_fn_down = false;

    if (left_fn_target == FN_TARGET_SYM && sym_fn_locked) {
        left_fn_target = FN_TARGET_NONE;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    int ret = deactivate_target(left_fn_target);
    left_fn_target = FN_TARGET_NONE;
    if (ret < 0) {
        LOG_ERR("Failed to deactivate Left Fn layer: %d", ret);
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fn_right_pressed(struct zmk_behavior_binding_event event) {
    right_fn_down = true;

    if (zmk_keymap_layer_active(LOCK_LAYER)) {
        right_fn_target = FN_TARGET_UNLOCK_RIGHT;
    } else if (zmk_keymap_layer_active(SYM_LAYER) && left_fn_down) {
        right_fn_target = FN_TARGET_CONFIG;
    } else {
        right_fn_target = FN_TARGET_UPPER;
    }

    int ret = activate_target(right_fn_target);
    if (ret < 0) {
        LOG_ERR("Failed to activate Right Fn layer: %d", ret);
        right_fn_down = false;
        right_fn_target = FN_TARGET_NONE;
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fn_right_released(struct zmk_behavior_binding_event event) {
    right_fn_down = false;

    int ret = deactivate_target(right_fn_target);
    right_fn_target = FN_TARGET_NONE;
    if (ret < 0) {
        LOG_ERR("Failed to deactivate Right Fn layer: %d", ret);
        return ret;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fn_lock_sym_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    if (!left_fn_down || right_fn_down) {
        sym_lock_key_consumed = false;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    sym_fn_locked = !sym_fn_locked;
    sym_lock_key_consumed = true;
    LOG_INF("SYM Fn Lock %s", sym_fn_locked ? "enabled" : "disabled");
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
    if (sym_fn_locked) {
        zmk_backlight_flash();
    }
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_fn_lock_sym_released(struct zmk_behavior_binding_event event) {
    bool consumed = sym_lock_key_consumed;
    sym_lock_key_consumed = false;
    return consumed ? ZMK_BEHAVIOR_OPAQUE : ZMK_BEHAVIOR_TRANSPARENT;
}

static int on_fn_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case FN_LEFT:
        return on_fn_left_pressed(event);
    case FN_RIGHT:
        return on_fn_right_pressed(event);
    case FN_LOCK_SYM:
        return on_fn_lock_sym_pressed(binding, event);
    default:
        LOG_ERR("Unknown Fn action: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int on_fn_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case FN_LEFT:
        return on_fn_left_released(event);
    case FN_RIGHT:
        return on_fn_right_released(event);
    case FN_LOCK_SYM:
        return on_fn_lock_sym_released(event);
    default:
        LOG_ERR("Unknown Fn action: %d", binding->param1);
        return -ENOTSUP;
    }
}

static const struct behavior_driver_api behavior_fn_driver_api = {
    .binding_pressed = on_fn_pressed,
    .binding_released = on_fn_released,
};

static int behavior_fn_init(const struct device *dev) { return 0; }

BEHAVIOR_DT_INST_DEFINE(0, behavior_fn_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_fn_driver_api);
