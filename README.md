# ZMK for the uConsole trackpad keyboard

This is the firmware source behind
[jhob101/uconsole-bb9900-keyboard](https://github.com/jhob101/uconsole-bb9900-keyboard), the keymap and build
config for the [hack2you uConsole trackpad keyboard kit](https://hack2you.tech/products/uconsole-trackpad-keyboard).
If you only want to build, flash or remap the keyboard, start there. This repo
holds the C code that the keymap alone cannot express.

It is for the V2.1 keyboard and does not work on V1.1. For a V1.1 keyboard, use
[noodleboy91/uconsole-bb9900-keyboard-v1.1](https://github.com/noodleboy91/uconsole-bb9900-keyboard-v1.1).
Other versions are untested.

It is **not** a copy of upstream ZMK, and that repo will not build without it.

## Branches

| Branch | What it is |
| --- | --- |
| `main` | The keyboard firmware. `uconsole-bb9900-keyboard`'s `config/west.yml` builds against this branch. |
| `upstream-main` | Upstream ZMK's `main` as it was when this fork was made. Kept for reference only. |

`main` here does not follow upstream ZMK's `main`. It is based on a December
2023 snapshot of ZMK, so current ZMK documentation and modules may not apply.

## Where the code comes from

Four layers, each a fork of the one before:

1. **[ZMK](https://github.com/zmkfirmware/zmk)**, the open-source keyboard firmware.
2. **[ZitaoTech/zmk](https://github.com/ZitaoTech/zmk)** (`bbkeyboard_tp`), which adds
   the BB9900 keyboard hardware: the board definition and the A320 optical
   trackpad driver.
3. **[thoughtfix/zmk](https://github.com/thoughtfix/zmk)** (`bbkeyboard_tp-fix9900`),
   which fixed trackpad scrolling and added the volume key behavior.
4. **This fork**, which adds the features listed below.

## What this fork adds

All of it is used by the `uconsole-bb9900-keyboard` keymap. File paths are under `app/`.

### Hold-to-scroll

`zmk,behavior-scroll-hold` (`src/behaviors/behavior_scroll_hold.c`). While a key
bound to it is held, trackpad motion scrolls instead of moving the pointer.

- By default, releasing the key without having moved the trackpad taps the
  bound behavior, so the key keeps a function of its own.
- With `pass-through`, the bound behavior is pressed and released along with
  the key instead, so it responds on press and can be held.
- With `toggle`, the key is a scroll switch: each press turns scrolling on or
  off, and it stays that way until the next press. The bound behavior is not
  tapped, so bind `&none`. A switch and a hold key can be used together, and
  releasing the hold key leaves the switch as it was.

This replaces thoughtfix's click-the-trackpad-to-toggle scroll mode. The axis
locking and batched scroll reports in `src/main.c` are kept.

Scroll motion is accumulated in fractions of a tick, so scroll speed follows
finger speed. Before, any motion in a poll counted as a whole tick, which put a
floor under the scroll rate. `CONFIG_TRACKPAD_SCROLL_SPEED` scales scrolling in
percent, independent of the pointer speed.

### USB joystick

Enabled with `CONFIG_ZMK_JOYSTICK=y`.

- HID report ID 4: a joystick with X and Y axes (signed 8 bit) and six buttons.
- `&jb N` presses joystick button N, counted from 0.
- `&jd JD_LEFT`, `JD_RIGHT`, `JD_UP`, `JD_DOWN` drive the axes from D-pad keys.
  When two opposite directions are held, the one pressed last wins.

It is sent over USB only. There is no Bluetooth characteristic for it.

### Trackpad on/off

`zmk,behavior-trackpad-enable` (`src/behaviors/behavior_trackpad_enable.c`)
switches the trackpad's pointer and scroll output off and on from the keymap.
The keyboard lock uses it. The state is not saved, so the trackpad is always on
after power-up.

### Backlight suspend and resume

`&bl BL_SUSPEND` holds the key backlight off without changing its on/off
setting, and `&bl BL_RESUME` releases it, so the backlight returns to whatever
it was. Nothing is saved as "off".

### Trackpad pointer fix

Pointer movement is clamped to the report's range. It used to overflow: a
scaled value past 127 wrapped to the opposite sign, so a fast movement, or the
noise a sensor produces as a finger lifts, threw the pointer backwards.

`CONFIG_INPUT_A320_OFN_ENGINE` sets the A320 sensor's `OFN_Engine` register
(0x60). It stays at `0x00`. The sensor's assert/de-assert (`0xA0`) was used as
lift detection for a few releases and then removed, because on some trackpads
it stopped a bare finger registering at all. The driver writes the register
even at `0x00`, to clear what an earlier firmware may have left in the sensor.

`CONFIG_INPUT_A320_DIAG` logs the sensor's surface quality, shutter and pixel
readings next to the motion it reports. It needs `CONFIG_ZMK_USB_LOGGING` and
is not for everyday use.

### Trackpad light

`CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT` drives the trackpad light pin from scroll
mode: one level while a scroll-hold key is held or a scroll switch is on, the
other otherwise. On the V2.1 keyboard the light goes out while scrolling.
`CONFIG_ZMK_TRACKPAD_SCROLL_LIGHT_INVERT` swaps the two levels. Trackpad light
brightness cycling, backlight synchronization, and suspend breathing can also
be enabled independently of scroll mode.

### Volume key fix

`zmk,behavior-vol-shift` (from thoughtfix: volume down, or volume up with
Shift) now works with one-shot Shift keys. Before, Shift+volume left a one-shot
Shift armed, and the next plain press went up as well.

## Building

This repo is not built on its own. Follow the build instructions in
[jhob101/uconsole-bb9900-keyboard](https://github.com/jhob101/uconsole-bb9900-keyboard#building), which fetches
this repo for you.

To try a change here, push it to a branch and point `revision:` in
`uconsole-bb9900-keyboard`'s `config/west.yml` at that branch.

## Licence

MIT, the same as ZMK. See [LICENSE](LICENSE).

The changes in this fork were written with the help of Claude (Anthropic) and
tested on one V2.1 keyboard. Treat them accordingly.
