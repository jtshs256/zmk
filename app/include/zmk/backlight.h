/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

int zmk_backlight_on();
int zmk_backlight_off();
int zmk_backlight_toggle();
void zmk_backlight_flash(void);
int zmk_backlight_toggle_auto_off_idle();
int zmk_backlight_suspend();
/** Fade to dark over duration_ms, then suspend while preserving the backlight state. */
int zmk_backlight_suspend_fade(uint32_t duration_ms);
int zmk_backlight_resume();
/** Finish active brightness transitions on user activity. */
void zmk_backlight_skip_fade_on_activity(void);
/** True when idle auto-off turned off a backlight that was previously on. */
bool zmk_backlight_was_idle_auto_off(void);
/** Restore the idle-auto-off state saved before a host lock, if still allowed. */
int zmk_backlight_wake_from_idle(bool was_idle_auto_off);
bool zmk_backlight_is_on();

int zmk_backlight_set_brt(uint8_t brightness);
uint8_t zmk_backlight_get_brt();
uint8_t zmk_backlight_calc_brt(int direction);
uint8_t zmk_backlight_calc_brt_cycle();
