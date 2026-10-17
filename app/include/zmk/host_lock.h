/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep in sync with the host-lock layers in the keyboard keymap. */
#define ZMK_HOST_LOCK_LAYER 5
#define ZMK_HOST_UNLOCK_LEFT_LAYER 6
#define ZMK_HOST_UNLOCK_RIGHT_LAYER 7

bool zmk_host_lock_active(void);
int zmk_host_lock_force_unlock(void);
int zmk_host_lock_engage(uint32_t fade_ms);
void zmk_host_lock_release(bool restore_backlight);
