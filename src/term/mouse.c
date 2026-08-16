#include "ghostcon/term/mouse.h"

#include <stdio.h>

static int
button_base_code(ghostcon_mouse_button_t button)
{
    switch (button) {
    case GC_MOUSE_LEFT:       return 0;
    case GC_MOUSE_MIDDLE:     return 1;
    case GC_MOUSE_RIGHT:      return 2;
    case GC_MOUSE_WHEEL_UP:   return 64;
    case GC_MOUSE_WHEEL_DOWN: return 65;
    case GC_MOUSE_NONE:
    default:
        return 3; /* no button -- X10 release code, or bare motion (mode 1003) */
    }
}

size_t
ghostcon_mouse_encode(const ghostcon_screen_t *screen,
                       ghostcon_mouse_button_t button,
                       ghostcon_mouse_action_t action,
                       GhosttyMods mods, int col, int row,
                       char *buf, size_t buf_len)
{
    if (!screen->mouse_tracking)
        return 0;

    bool is_wheel = (button == GC_MOUSE_WHEEL_UP || button == GC_MOUSE_WHEEL_DOWN);

    if (action == GC_MOUSE_MOTION && !is_wheel) {
        if (screen->mouse_protocol == 1002 && button == GC_MOUSE_NONE)
            return 0; /* drag-only mode: motion without a held button isn't reported */
        if (screen->mouse_protocol != 1002 && screen->mouse_protocol != 1003)
            return 0; /* mode 1000 (click-only), or an unset/unknown protocol, never reports motion */
    }

    int mod_bits = 0;
    if (mods & GHOSTTY_MODS_SHIFT) mod_bits += 4;
    if (mods & GHOSTTY_MODS_ALT)   mod_bits += 8;
    if (mods & GHOSTTY_MODS_CTRL)  mod_bits += 16;
    if (action == GC_MOUSE_MOTION) mod_bits += 32;

    /* SGR distinguishes release purely via the trailing 'M'/'m', so it
       carries the actual button number through on release too (matches
       xterm/kitty/alacritty). Legacy X10 has no such signal -- release
       is always base code 3 regardless of which button. */
    int base = (action == GC_MOUSE_RELEASE && !screen->mouse_sgr)
                   ? 3 : button_base_code(button);
    int cb = base + mod_bits;

    int col1 = col + 1; /* 1-based for the wire protocol */
    int row1 = row + 1;

    if (screen->mouse_sgr) {
        char final = (action == GC_MOUSE_RELEASE) ? 'm' : 'M';
        int n = snprintf(buf, buf_len, "\x1b[<%d;%d;%d%c", cb, col1, row1, final);
        if (n < 0 || (size_t)n >= buf_len)
            return 0;
        return (size_t)n;
    }

    /* Legacy X10: 3 raw bytes after "ESC[M", each value+32, clamped to
       the single-byte range (255-32=223 max meaningful value) -- no
       way to represent larger coordinates or a wider button range in
       this framing; callers should prefer SGR (screen->mouse_sgr,
       DECSET 1006) whenever the app has requested it. */
    if (buf_len < 6)
        return 0;
    if (cb > 223) cb = 223;
    if (col1 > 223) col1 = 223;
    if (row1 > 223) row1 = 223;

    buf[0] = 0x1b;
    buf[1] = '[';
    buf[2] = 'M';
    buf[3] = (char)(32 + cb);
    buf[4] = (char)(32 + col1);
    buf[5] = (char)(32 + row1);
    return 6;
}
