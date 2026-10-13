/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Parameters for zmk,behavior-trackpad-light */
#define TPL_OFF 0
#define TPL_ON 1
#define TPL_TOG 2
#define TPL_CYCLE 5
/* Hold the light off without changing the saved choice, for the keyboard
 * lock, and let it go again. */
#define TPL_SUSPEND 3
#define TPL_RESUME 4
