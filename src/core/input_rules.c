/* input_rules.c
 *
 * The input decisions that do not need libinput -- see
 * include/ghostcon/core/input_rules.h.
 */

#include "ghostcon/core/input_rules.h"

#include <linux/input-event-codes.h>
#include <math.h>

ghostcon_shortcut_t
ghostcon_input_match_shortcut(uint32_t evdev_code, GhosttyMods mods, GhosttyKeyAction action)
{
    if (action != GHOSTTY_KEY_ACTION_PRESS)
        return GC_SHORTCUT_NONE;

    /* Shift+Up/Down/PageUp/PageDown: the scrollback view shortcuts, mirroring
     * kmscon's grab-scroll-up/-down/grab-page-up/-down defaults.
     *
     * Exact Shift-only match (not "Shift held among others").
     */
    if (mods == GHOSTTY_MODS_SHIFT)
    {
        switch (evdev_code)
        {
        case KEY_UP:       return GC_SHORTCUT_SCROLL_UP;
        case KEY_DOWN:     return GC_SHORTCUT_SCROLL_DOWN;
        case KEY_PAGEUP:   return GC_SHORTCUT_PAGE_UP;
        case KEY_PAGEDOWN: return GC_SHORTCUT_PAGE_DOWN;
        default: break;
        }
    }

    /* Ctrl+=/Ctrl+Minus: the zoom shortcuts, mirroring kmscon's
     * grab-zoom-in/grab-zoom-out defaults (kmscon documents them as
     * <Ctrl>Plus/<Ctrl>Minus; bound here to the unshifted '='/'-' keys instead,
     * matching how most terminals/browsers do Ctrl+= to zoom in without requiring
     * Shift too).
     *
     * KEY_KPPLUS/KEY_KPMINUS are aliases for the numpad.
     */
    if (mods == GHOSTTY_MODS_CTRL)
    {
        switch (evdev_code)
        {
        case KEY_EQUAL:
        case KEY_KPPLUS:  return GC_SHORTCUT_ZOOM_IN;
        case KEY_MINUS:
        case KEY_KPMINUS: return GC_SHORTCUT_ZOOM_OUT;
        default: break;
        }
    }

    /* Ctrl+Alt+D: dump the live screen state to a fixed diagnostic file
     * (core/main.c does the actual write, since it owns app->term).
     *
     * Added to debug a rendering bug that only reproduces in a real, long-running
     * session -- it lets you capture the exact live cell state at the moment a
     * glitch is visible, for a direct diff against tools/ghostcon_dump.c's
     * reference output, instead of guessing at a reproduction offline.
     *
     * Unlike the shortcuts above, this one asks for Ctrl and Alt *among* the
     * modifiers: it is a diagnostic chord, not a terminal-mode chord.
     */
    if ((mods & GHOSTTY_MODS_CTRL) && (mods & GHOSTTY_MODS_ALT) && evdev_code == KEY_D)
        return GC_SHORTCUT_DUMP;

    return GC_SHORTCUT_NONE;
}

bool ghostcon_input_match_binding(uint32_t evdev_code, GhosttyMods mods, GhosttyKeyAction action, GhosttyMods want_mods, uint32_t want_evdev)
{
    if (action != GHOSTTY_KEY_ACTION_PRESS)
        return false;
    return mods == want_mods && evdev_code == want_evdev;
}

int ghostcon_input_scroll_accumulate(double *accum, double value, float speed, bool is_wheel)
{
    /* v120 for a real wheel (per-detent already discretized by libinput); plain
     * scroll_value for touchpad two-finger scroll (a continuous pixel-ish
     * distance)
     *
     * the wheel's own detent count is what a single flick can report (a single
     * flick can report multiple 120s); touchpad two-finger scroll has no natural
     * "detent" concept, so each event with a nonzero value simply counts as one
     * raw tick, same simplification the pre-scroll_speed code already made.
     */
    double raw_ticks = is_wheel ? fabs(value) / 120.0 : 1.0;

    /* A direction reversal wipes any built-up fractional backlog from the
     * opposite direction
     *
     * otherwise a throttled (speed<1) scroll that reverses could fire an
     * unexpected extra tick from leftover accumulation.
     */
    if (*accum != 0.0 && (value > 0) != (*accum > 0))
        *accum = 0.0;
    *accum += (value > 0 ? 1.0 : -1.0) * raw_ticks * (double)speed;

    int ticks = 0;
    while (fabs(*accum) >= 1.0)
    {
        ticks++;
        *accum += (*accum > 0) ? -1.0 : 1.0;
    }
    return ticks;
}

void ghostcon_input_clamp_pointer(int *x, int *y, int viewport_w, int viewport_h)
{
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
    if (*x >= viewport_w)
        *x = viewport_w - 1;
    if (*y >= viewport_h)
        *y = viewport_h - 1;
}
