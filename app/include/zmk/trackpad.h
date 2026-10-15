/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Shared state between keymap behaviors (set from key events) and the trackpad
 * polling loop in main.c (which turns motion into cursor and scroll reports).
 */

/**
 * True while trackpad motion should scroll: a key bound to the scroll-hold
 * behavior is held down, or a scroll switch (its `toggle` option) is on.
 */
bool zmk_trackpad_scroll_held(void);

/**
 * Called by the trackpad loop when it sees motion while scroll is held, so
 * the key's release is treated as the end of a scroll rather than a tap.
 */
void zmk_trackpad_scroll_mark_used(void);

/**
 * Sets the trackpad light for the given scroll state. Only built with
 * CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT.
 */
void zmk_trackpad_scroll_light_update(bool scrolling);

/** True when the user's saved trackpad light brightness is nonzero. */
bool zmk_trackpad_light_enabled(void);
void zmk_trackpad_light_set_enabled(bool on);
/** Get or set the user's saved trackpad light brightness, from 0 to 100. */
uint8_t zmk_trackpad_light_get_brightness(void);
void zmk_trackpad_light_set_brightness(uint8_t brightness);

/** Holds the light off while true, without changing the saved choice. */
#if defined(CONFIG_ZMK_TRACKPAD_LIGHT)
void zmk_trackpad_light_set_suspended(bool suspended);
#else
static inline void zmk_trackpad_light_set_suspended(bool suspended) { (void)suspended; }
#endif

/** Sync backlight level with keyboard. */
void zmk_trackpad_light_backlight_sync(void);

/** False while the trackpad is switched off by the trackpad-enable behavior. */
bool zmk_trackpad_enabled(void);
void zmk_trackpad_set_enabled(bool enabled);
