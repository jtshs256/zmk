/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Shared ramp timing for idle auto-off, Kana host-lock and wake transitions. */
#define ZMK_LED_FADE_DURATION_MS 1000
#define ZMK_LED_FADE_INTERVAL_MS 20
#define ZMK_LED_WAKE_FADE_DURATION_MS 300
#define ZMK_MANUAL_LOCK_FADE_DURATION_MS ZMK_LED_WAKE_FADE_DURATION_MS
