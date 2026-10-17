/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define BL_ON_CMD 0
#define BL_OFF_CMD 1
#define BL_TOG_CMD 2
#define BL_INC_CMD 3
#define BL_DEC_CMD 4
#define BL_CYCLE_CMD 5
#define BL_SET_CMD 6
#define BL_SUSPEND_CMD 7
#define BL_RESUME_CMD 8
#define BL_AUTO_OFF_TOG_CMD 9

#define BL_ON BL_ON_CMD 0
#define BL_OFF BL_OFF_CMD 0
#define BL_TOG BL_TOG_CMD 0
#define BL_INC BL_INC_CMD 0
#define BL_DEC BL_DEC_CMD 0
#define BL_CYCLE BL_CYCLE_CMD 0
#define BL_SET BL_SET_CMD
/* Hold the backlight off without changing its on/off state, and let it go again. */
#define BL_SUSPEND BL_SUSPEND_CMD 0
#define BL_RESUME BL_RESUME_CMD 0
#define BL_AUTO_OFF_TOG BL_AUTO_OFF_TOG_CMD 0
