#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ghostty/vt/key.h>

/* The input decisions that do not need libinput.
 *
 * Everything here is a pure function of the key or pointer event, so it can
 * be tested without a device, a seat, or a VT -- see
 * tests/test_input_rules.c.
 *
 * The rest of the input path (core/input.c) is the part that needs libinput
 * and xkbcommon: reading the device, decoding the key, and reporting to the
 * PTY.
 */

/* The shortcuts the terminal consumes itself, rather than forwarding them to
 * the program running in the PTY.
 */
typedef enum
{
    GC_SHORTCUT_NONE,
    GC_SHORTCUT_SCROLL_UP,   /* Shift+Up: scroll the view back */
    GC_SHORTCUT_SCROLL_DOWN, /* Shift+Down: scroll the view forward */
    GC_SHORTCUT_PAGE_UP,     /* Shift+PageUp: a page back */
    GC_SHORTCUT_PAGE_DOWN,   /* Shift+PageDown: a page forward */
    GC_SHORTCUT_ZOOM_IN,     /* Ctrl+= : one zoom tick in */
    GC_SHORTCUT_ZOOM_OUT,    /* Ctrl+- : one zoom tick out */
    GC_SHORTCUT_DUMP,        /* Ctrl+Alt+D: dump the live screen state */
} ghostcon_shortcut_t;

/* Which shortcut a key event triggers, if any.
 *
 * Press-only: a release never triggers a shortcut.
 *
 * The scroll and zoom shortcuts match the modifiers *exactly* (not "held among
 * others"), the same specificity kmscon's own default grabs use, so an app that
 * wants Shift+Ctrl+Up for something else still gets it forwarded normally.
 *
 * The dump shortcut is the exception: it asks for Ctrl and Alt *among* the
 * modifiers, because it is a diagnostic chord rather than a terminal-mode chord.
 */
extern ghostcon_shortcut_t ghostcon_input_match_shortcut(uint32_t evdev_code, GhosttyMods mods, GhosttyKeyAction action);

/* Whether a key event matches a configured binding (the copy and paste
 * shortcuts).
 *
 * Exact mods match (not "held among others") and press-only, the same
 * specificity the built-in shortcuts use.
 */
extern bool ghostcon_input_match_binding(uint32_t evdev_code, GhosttyMods mods, GhosttyKeyAction action, GhosttyMods want_mods, uint32_t want_evdev);

/* Adds a scroll value to the scroll-speed accumulator, and reports how many whole
 * ticks the accumulator now holds.
 *
 * The accumulator is fractional: a wheel reports 1/120th of a detent per v120
 * unit, and a touchpad reports a continuous distance, so a tick is only emitted
 * once the accumulator reaches a whole one.
 *
 * `speed` scales the accumulation (a slow scroll accumulates more slowly).
 *
 * `is_wheel` selects the v120 detent conversion; a touchpad's value is a plain
 * distance and counts as one raw tick.
 *
 * A direction reversal wipes the fractional backlog built up in the opposite
 * direction, so a throttled scroll that reverses cannot fire an unexpected extra
 * tick from it.
 *
 * The caller consumes the ticks (one mouse report each) and subtracts one from
 * the accumulator per tick.
 */
extern int ghostcon_input_scroll_accumulate(double *accum, double value, float speed, bool is_wheel);

/* Clamps a pointer position to the viewport.
 *
 * The pointer is reported in pixels, and a device can report a position
 * outside the viewport (a drag that ran off the edge, an absolute device
 * that reports its own range).
 */
extern void ghostcon_input_clamp_pointer(int *x, int *y, int viewport_w, int viewport_h);
