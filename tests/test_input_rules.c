/* test_input_rules
 *
 * The input decisions that do not need libinput.
 *
 * core/input.c reads a real device, so the parts of it that decide *what* a key
 * or a scroll does -- which shortcut a chord triggers, how a scroll accumulates
 * into whole ticks, where a pointer is clamped to -- live in core/input_rules.c,
 * where they are plain functions and can be checked here.
 *
 * What this covers that nothing else did: the scroll-speed accumulator's
 * fractional arithmetic (including the direction-reversal reset, which is easy to
 * get wrong and invisible until a slow scroll reverses), and the exact-modifier
 * rule the shortcuts use.
 */

#include "ghostcon/core/input_rules.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static bool close_to(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 0.0001;
}

int main(void)
{
    /* --- the scroll shortcuts --- */
    CHECK(ghostcon_input_match_shortcut(KEY_UP, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_SCROLL_UP,
          "Shift+Up scrolls the view back");
    CHECK(ghostcon_input_match_shortcut(KEY_DOWN, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_SCROLL_DOWN,
          "Shift+Down scrolls the view forward");
    CHECK(ghostcon_input_match_shortcut(KEY_PAGEUP, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_PAGE_UP,
          "Shift+PageUp goes a page back");
    CHECK(ghostcon_input_match_shortcut(KEY_PAGEDOWN, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_PAGE_DOWN,
          "Shift+PageDown goes a page forward");

    /* --- the zoom shortcuts --- */
    CHECK(ghostcon_input_match_shortcut(KEY_EQUAL, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_ZOOM_IN,
          "Ctrl+= zooms in");
    CHECK(ghostcon_input_match_shortcut(KEY_KPPLUS, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_ZOOM_IN,
          "Ctrl+numpad-plus zooms in");
    CHECK(ghostcon_input_match_shortcut(KEY_MINUS, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_ZOOM_OUT,
          "Ctrl+- zooms out");
    CHECK(ghostcon_input_match_shortcut(KEY_KPMINUS, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_ZOOM_OUT,
          "Ctrl+numpad-minus zooms out");

    /* --- the dump shortcut --- */
    CHECK(ghostcon_input_match_shortcut(KEY_D, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_DUMP,
          "Ctrl+Alt+D dumps the screen state");
    CHECK(ghostcon_input_match_shortcut(KEY_D, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT | GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_DUMP,
          "Ctrl+Alt+Shift+D still dumps (the dump chord asks for Ctrl and Alt among the modifiers)");
    CHECK(ghostcon_input_match_shortcut(KEY_D, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "Ctrl+D on its own is not the dump chord (Alt is part of it)");
    CHECK(ghostcon_input_match_shortcut(KEY_D, GHOSTTY_MODS_ALT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "Alt+D on its own is not the dump chord (Ctrl is part of it)");

    /* --- what is not a shortcut --- */
    CHECK(ghostcon_input_match_shortcut(KEY_UP, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_RELEASE) == GC_SHORTCUT_NONE,
          "a release never triggers a shortcut");
    CHECK(ghostcon_input_match_shortcut(KEY_UP, 0, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "a bare arrow key is not a shortcut");
    CHECK(ghostcon_input_match_shortcut(KEY_UP, GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "Shift+Ctrl+Up is not a shortcut (the modifiers must match exactly)");
    CHECK(ghostcon_input_match_shortcut(KEY_A, GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "Shift+A is not a shortcut");
    CHECK(ghostcon_input_match_shortcut(KEY_EQUAL, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS) == GC_SHORTCUT_NONE,
          "Ctrl+Shift+= is not a zoom shortcut (the modifiers must match exactly)");

    /* --- the configured bindings (copy and paste) --- */
    CHECK(ghostcon_input_match_binding(KEY_C, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_PRESS,
                                        GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, KEY_C),
          "a configured binding matches its exact modifier set");
    CHECK(!ghostcon_input_match_binding(KEY_C, GHOSTTY_MODS_CTRL, GHOSTTY_KEY_ACTION_PRESS,
                                         GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, KEY_C),
          "a configured binding does not match a subset of its modifiers");
    CHECK(!ghostcon_input_match_binding(KEY_C, GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, GHOSTTY_KEY_ACTION_RELEASE,
                                         GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT, KEY_C),
          "a configured binding is press-only");

    /* --- the scroll-speed accumulator --- */
    {
        /* A touchpad value is a plain distance: one raw tick per event. */
        double accum = 0.0;
        int ticks = ghostcon_input_scroll_accumulate(&accum, 1.0, 1.0f, false);
        CHECK(ticks == 1 && close_to(accum, 0.0),
              "a touchpad scroll of one full unit is one tick, with nothing left over");

        /* A wheel reports 1/120th of a detent per v120 unit, so 120 units
         * make one tick. */
        accum = 0.0;
        ticks = ghostcon_input_scroll_accumulate(&accum, 120.0, 1.0f, true);
        CHECK(ticks == 1 && close_to(accum, 0.0),
              "a wheel detent (120 v120 units) is one tick");

        accum = 0.0;
        ticks = ghostcon_input_scroll_accumulate(&accum, 60.0, 1.0f, true);
        CHECK(ticks == 0 && close_to(accum, 0.5),
              "half a detent accumulates without emitting a tick");

        accum = 0.0;
        ghostcon_input_scroll_accumulate(&accum, 60.0, 1.0f, true);
        ticks = ghostcon_input_scroll_accumulate(&accum, 60.0, 1.0f, true);
        CHECK(ticks == 1 && close_to(accum, 0.0),
              "the other half of the detent completes it");

        /* A slow scroll accumulates more slowly. */
        accum = 0.0;
        ticks = ghostcon_input_scroll_accumulate(&accum, 120.0, 0.5f, true);
        CHECK(ticks == 0 && close_to(accum, 0.5),
              "a half-speed scroll takes two detents to make a tick");

        /* A direction reversal wipes the fractional backlog, so a throttled
         * scroll that reverses cannot fire an extra tick from it. */
        accum = 0.0;
        ghostcon_input_scroll_accumulate(&accum, 60.0, 1.0f, true);
        ticks = ghostcon_input_scroll_accumulate(&accum, -60.0, 1.0f, true);
        CHECK(ticks == 0 && close_to(accum, -0.5),
              "a direction reversal drops the backlog built up in the other direction");

        /* A big flick reports several detents at once. */
        accum = 0.0;
        ticks = ghostcon_input_scroll_accumulate(&accum, 360.0, 1.0f, true);
        CHECK(ticks == 3 && close_to(accum, 0.0),
              "three detents in one event are three ticks");

        /* A negative wheel value scrolls the other way. */
        accum = 0.0;
        ticks = ghostcon_input_scroll_accumulate(&accum, -120.0, 1.0f, true);
        CHECK(ticks == 1 && close_to(accum, 0.0),
              "a backwards detent is one tick too");
    }

    /* --- the pointer clamp --- */
    {
        int x = 5, y = 7;
        ghostcon_input_clamp_pointer(&x, &y, 100, 50);
        CHECK(x == 5 && y == 7, "a pointer inside the viewport is left alone");

        x = -3;
        y = -1;
        ghostcon_input_clamp_pointer(&x, &y, 100, 50);
        CHECK(x == 0 && y == 0, "a pointer off the top-left is clamped to it");

        x = 100;
        y = 50;
        ghostcon_input_clamp_pointer(&x, &y, 100, 50);
        CHECK(x == 99 && y == 49, "a pointer off the bottom-right is clamped to the last pixel");

        x = 1000;
        y = 1000;
        ghostcon_input_clamp_pointer(&x, &y, 100, 50);
        CHECK(x == 99 && y == 49, "a pointer far outside the viewport is clamped to it");
    }

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
