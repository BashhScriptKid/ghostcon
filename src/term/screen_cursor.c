#include "ghostcon/term/screen.h"
#include "ghostcon/term/screen_cursor.h"
#include "screen_internal.h"
#include <stdlib.h>
#include <string.h>

/* Cursor movement */

/* CUU/CUD (cursor up/down) clamp to the scroll region ONLY under origin mode
 * (DECOM)
 *
 * otherwise (the common case, DECOM off) they clamp to the full screen, same
 * distinction ghostcon_screen_cursor_ vertical_abs() (VPA) already gets right.
 *
 * Found live: a real, well-known idiom
 *
 * CSI H then a large CSI B to jump to the absolute screen bottom regardless of
 * exact height
 *
 * landed short by however many rows a scroll region reserved at the bottom (e.g.
 * a fixed status line below a chat log's scroll region), while CUP/VPA-based
 * positioning to that same row worked correctly, producing the exact same text
 * rendered on two different rows.
 */
/* Fuzzer-found (hardening plan #5): all four of these cast the raw uint16_t n (up
 * to UINT16_MAX from a CSI param) straight to int16_t before the add/subtract.
 *
 * For n >= 32768 that cast wraps negative, silently REVERSING the intended
 * direction
 *
 * and each function only ever clamped the bound it expected to hit from the
 * correct direction, leaving the other bound (now reachable because the sign
 * flipped) completely unchecked.
 *
 * Minimized repro: CSI 64614 a (HPR, same handler as CUF) walked cursor.x to -922
 * with no lower-bound check, and the next printable character wrote clean off the
 * front of the row's cells array. int32_t arithmetic sidesteps the wrap; clamping
 * both bounds unconditionally (not just the "expected" one) closes the
 * reachable-from-the-other-side gap regardless.
 */
void ghostcon_screen_cursor_up(ghostcon_screen_t *s, uint16_t n)
{
    int32_t min = s->origin_mode ? s->scroll_region.top : 0;
    int32_t y = (int32_t)s->cursor.y - (int32_t)n;
    if (y < min) y = min;
    if (y > (int32_t)s->rows_visible - 1) y = (int32_t)s->rows_visible - 1;
    s->cursor.y = (int16_t)y;
}

void ghostcon_screen_cursor_down(ghostcon_screen_t *s, uint16_t n)
{
    int32_t max = s->origin_mode ? s->scroll_region.bottom : (int32_t)s->rows_visible - 1;
    int32_t y = (int32_t)s->cursor.y + (int32_t)n;
    if (y > max) y = max;
    if (y < 0) y = 0;
    s->cursor.y = (int16_t)y;
}

void ghostcon_screen_cursor_left(ghostcon_screen_t *s, uint16_t n)
{
    int32_t x = (int32_t)s->cursor.x - (int32_t)n;
    if (x < 0) x = 0;
    if (x > (int32_t)s->cols - 1) x = (int32_t)s->cols - 1;
    s->cursor.x = (int16_t)x;
}

void ghostcon_screen_cursor_right(ghostcon_screen_t *s, uint16_t n)
{
    int32_t x = (int32_t)s->cursor.x + (int32_t)n;
    if (x >= (int32_t)s->cols) x = (int32_t)s->cols - 1;
    if (x < 0) x = 0;
    s->cursor.x = (int16_t)x;
}

void ghostcon_screen_cursor_set(ghostcon_screen_t *s, int16_t x, int16_t y)
{
    if (s->origin_mode)
    {
        x += s->margin_region.left;
        y += s->scroll_region.top;
    }
    if (x < 0) x = 0;
    if (x >= (int16_t)s->cols) x = (int16_t)(s->cols - 1);
    if (y < 0) y = 0;
    if (y >= (int16_t)s->rows_visible) y = (int16_t)(s->rows_visible - 1);
    s->cursor.x = x;
    s->cursor.y = y;
    s->cursor.pending_wrap = false;
}

void ghostcon_screen_cursor_horizontal_abs(ghostcon_screen_t *s, int16_t x)
{
    if (s->origin_mode)
        x += s->margin_region.left;
    if (x < 0) x = 0;
    if (x >= (int16_t)s->cols) x = (int16_t)(s->cols - 1);
    s->cursor.x = x;
}

void ghostcon_screen_cursor_vertical_abs(ghostcon_screen_t *s, int16_t y)
{
    /* VPA (CSI Ps d): row absolute, column unchanged. With origin mode the
     * row is relative to the scroll region and clamped to it (mirrors
     * Ghostty's setCursorPos).
     */
    int16_t y_max = (int16_t)(s->rows_visible - 1);
    if (s->origin_mode)
    {
        y += s->scroll_region.top;
        y_max = s->scroll_region.bottom;
    }
    if (y < 0) y = 0;
    if (y > y_max) y = y_max;
    s->cursor.y = y;
    s->cursor.pending_wrap = false;
}

void ghostcon_screen_cursor_next_line(ghostcon_screen_t *s)
{
    /* Only scroll the region if the cursor was actually AT its bottom edge before
     * moving (mirrors soft_wrap()'s `==` check)
     *
     * a cursor sitting below/outside the scroll region entirely (e.g. a fixed
     * status/input area a TUI deliberately excludes from the scroll region) must
     * just clamp to the full screen, never scroll the region above it.
     *
     * The old `y++` then `> bottom` check fired for that out-of-region case too,
     * spuriously scrolling the main content on every cursor-next-line issued from
     * below the region
     *
     * found live: exactly this, corrupting content above a fixed bottom input box
     * on every animation tick that redrew it via CNL.
     */
    bool was_at_region_bottom = (s->cursor.y == s->scroll_region.bottom);
    s->cursor.x = s->margin_region.left;
    if (was_at_region_bottom)
    {
        ghostcon_screen_cursor_scroll_up(s);
    }
    else
    {
        int16_t max = s->origin_mode ? s->scroll_region.bottom : (int16_t)(s->rows_visible - 1);
        s->cursor.y++;
        if (s->cursor.y > max)
            s->cursor.y = max;
    }
    s->cursor.pending_wrap = false;
}

void ghostcon_screen_cursor_prev_line(ghostcon_screen_t *s)
{
    s->cursor.x = s->margin_region.left;
    s->cursor.y--;
    if (s->cursor.y < s->scroll_region.top)
        s->cursor.y = s->scroll_region.top;
}

void ghostcon_screen_cursor_scroll_up(ghostcon_screen_t *s)
{
    ghostcon_screen_scroll_up(s, 1);
}

void ghostcon_screen_cursor_scroll_down(ghostcon_screen_t *s)
{
    ghostcon_screen_scroll_down(s, 1);
}

void ghostcon_screen_cursor_save(ghostcon_screen_t *s)
{
    s->saved_cursor.x = s->cursor.x;
    s->saved_cursor.y = s->cursor.y;
    s->saved_cursor.cursor_style = s->cursor.cursor_style;
    s->saved_cursor.pending_wrap = s->cursor.pending_wrap;
    s->saved_cursor.style_id = s->cursor.style_id;
    s->cursor_saved = true;
}

void ghostcon_screen_cursor_restore(ghostcon_screen_t *s)
{
    if (!s->cursor_saved) return;
    s->cursor.x = s->saved_cursor.x;
    s->cursor.y = s->saved_cursor.y;
    s->cursor.cursor_style = s->saved_cursor.cursor_style;
    s->cursor.pending_wrap = s->saved_cursor.pending_wrap;
    s->cursor.style_id = s->saved_cursor.style_id;
}

void ghostcon_screen_carriage_return(ghostcon_screen_t *s)
{
    s->cursor.x = s->margin_region.left;
    s->cursor.pending_wrap = false;
}

void ghostcon_screen_linefeed(ghostcon_screen_t *s)
{
    s->cursor.pending_wrap = false;
    if (s->cursor.y == s->scroll_region.bottom)
    {
        ghostcon_screen_scroll_up(s, 1);
    }
    else
    {
        s->cursor.y++;
    }
}

void ghostcon_screen_reverse_index(ghostcon_screen_t *s)
{
    if (s->cursor.y == s->scroll_region.top)
    {
        ghostcon_screen_scroll_down(s, 1);
    }
    else
    {
        /* Fuzzer-found SEGV: a cursor legitimately above the scroll region's top
         * (e.g.
         *
         * DECSTBM homes the cursor to screen row 0 when origin mode is off, which
         * can sit above a scroll region that doesn't start at 0) took this branch
         * and decremented with no lower bound
         *
         * repeatable RI calls walk cursor.y arbitrarily negative, and row_idx()'s
         * modulo on a negative index produces a huge out-of-bounds s->rows[]
         * access.
         *
         * Same clamp floor ghostcon_screen_cursor_up() already uses for the
         * identical "cursor above the effective top" case.
         */
        int16_t min = s->origin_mode ? s->scroll_region.top : 0;
        if (s->cursor.y > min)
            s->cursor.y--;
    }
}

void ghostcon_screen_tab(ghostcon_screen_t *s)
{
    /* Move to next tab stop, or to margin right if none */
    int16_t start = s->cursor.x + 1;
    for (int16_t i = start; i <= s->margin_region.right; i++)
    {
        if (i < (int16_t)s->tabstops.cols && s->tabstops.stops[i])
        {
            s->cursor.x = i;
            return;
        }
    }
    /* No tab stop found — go to margin right */
    s->cursor.x = s->margin_region.right;
}

void ghostcon_screen_tab_back(ghostcon_screen_t *s)
{
    int16_t start = s->cursor.x - 1;
    for (int16_t i = start; i >= 0; i--)
    {
        if (i < (int16_t)s->tabstops.cols && s->tabstops.stops[i])
        {
            s->cursor.x = i;
            return;
        }
    }
    s->cursor.x = 0;
}


/* Tab stops */

void ghostcon_screen_tab_clear(ghostcon_screen_t *s, int mode)
{
    /* mode 0 = clear at cursor, mode 3 = clear all */
    if (mode == 3)
    {
        memset(s->tabstops.stops, 0, s->tabstops.cols * sizeof(bool));
    }
    else
    {
        if (s->cursor.x < (int16_t)s->tabstops.cols)
            s->tabstops.stops[s->cursor.x] = false;
    }
}

void ghostcon_screen_tab_set(ghostcon_screen_t *s)
{
    if (s->cursor.x < (int16_t)s->tabstops.cols)
        s->tabstops.stops[s->cursor.x] = true;
}
