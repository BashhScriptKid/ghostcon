#include "ghostcon/term/screen.h"
#include "ghostcon/term/screen_edit.h"
#include "screen_internal.h"
#include <stdlib.h>
#include <string.h>

/* Text insertion */

/* Print a cell at the cursor position with the given wide state (mirrors
 * Ghostty's Terminal.printCell). Clears stale wide spacers left behind when
 * a cell's wide state changes.
 */
static void print_cell(ghostcon_screen_t *s, uint32_t cp, ghostcon_cell_wide_t wide)
{
    ghostcon_cell_t *cell = cell_at(s, s->cursor.x, s->cursor.y);

    if (ghostcon_cell_get_wide(*cell) != wide)
    {
        switch (ghostcon_cell_get_wide(*cell))
        {
        case GC_CELL_WIDE_NARROW:
            break;
        case GC_CELL_WIDE_WIDE:
            /* Clear the spacer tail to the right */
            if (s->cursor.x < (int16_t)s->cols - 1)
                cell_at(s, s->cursor.x + 1, s->cursor.y)->raw = 0;
            /* Clear a stale spacer_head at the end of the previous row */
            if (s->cursor.y > 0 && s->cursor.x <= 1)
            {
                ghostcon_cell_t *head = cell_at(s, (int16_t)s->cols - 1, s->cursor.y - 1);
                if (ghostcon_cell_get_wide(*head) == GC_CELL_WIDE_SPACER_HEAD)
                    ghostcon_cell_set_wide(head, GC_CELL_WIDE_NARROW);
            }
            break;
        case GC_CELL_WIDE_SPACER_TAIL:
            /* Clear the wide cell to the left */
            if (s->cursor.x > 0)
                cell_at(s, s->cursor.x - 1, s->cursor.y)->raw = 0;
            /* Clear a stale spacer_head at the end of the previous row */
            if (s->cursor.y > 0 && s->cursor.x <= 1)
            {
                ghostcon_cell_t *head = cell_at(s, (int16_t)s->cols - 1, s->cursor.y - 1);
                if (ghostcon_cell_get_wide(*head) == GC_CELL_WIDE_SPACER_HEAD)
                    ghostcon_cell_set_wide(head, GC_CELL_WIDE_NARROW);
            }
            break;
        case GC_CELL_WIDE_SPACER_HEAD:
            break;
        }
    }

    *cell = ghostcon_cell_make(cp);
    ghostcon_cell_set_wide(cell, wide);
    ghostcon_cell_set_style(cell, s->cursor.style_id);
    ghostcon_cell_set_semantic(cell, s->semantic_current);
    ghostcon_cell_set_hyperlink_id(cell, s->cursor.hyperlink_id);
    if (s->cursor.protected)
        ghostcon_cell_set_protected(cell, true);
    mark_dirty(s, s->cursor.y);
}

/* Soft wrap: move the cursor to the start of the next line, marking the row
 * as wrapped (mirrors Ghostty's printWrap).
 */
static void soft_wrap(ghostcon_screen_t *s)
{
    bool mark_wrap = (s->cursor.x == (int16_t)s->cols - 1);
    if (mark_wrap)
        s->rows[row_idx(s, s->cursor.y)].wrap = true;

    if (s->cursor.y == s->scroll_region.bottom)
    {
        ghostcon_screen_scroll_up(s, 1);
    }
    else
    {
        s->cursor.y++;
    }
    s->cursor.x = s->margin_region.left;
    s->cursor.pending_wrap = false;

    if (mark_wrap)
        s->rows[row_idx(s, s->cursor.y)].wrap_continuation = true;
}

void ghostcon_screen_put_char(ghostcon_screen_t *s, uint32_t codepoint)
{
    /* Our right margin depends where our cursor is now (Ghostty's
     * right_limit): if the cursor was placed past the margin by an
     * absolute-position command, wrap at the real screen edge.
     */
    int16_t right_limit = s->cursor.x > s->margin_region.right
        ? (int16_t)s->cols
        : (int16_t)(s->margin_region.right + 1);

    uint8_t width = ghostcon_unicode_width(codepoint);

    /* Zero-width characters are attached as grapheme data to the previous
     * cell (mirrors Ghostty's print).
     */
    if (width == 0)
    {
        int left = (s->auto_wrap && s->cursor.pending_wrap) ? 0 : 1;
        if (s->cursor.x == 0 && left == 1) return;

        ghostcon_cell_t *prev = cell_at(s, s->cursor.x - left, s->cursor.y);
        if (ghostcon_cell_get_wide(*prev) == GC_CELL_WIDE_SPACER_TAIL)
            prev = cell_at(s, s->cursor.x - left - 1, s->cursor.y);
        if (!ghostcon_cell_has_text(*prev)) return;

        /* No grapheme storage yet (Ghostty stores combining sequences on the
         * page)
         *
         * tag the cell so the renderer can resolve the full sequence once storage
         * lands.
         */
        ghostcon_cell_set_tag(prev, GC_CELL_CODEPOINT_GRAPHEME);
        s->rows[row_idx(s, s->cursor.y)].grapheme = true;
        mark_dirty(s, s->cursor.y);
        return;
    }

    /* Handle pending soft-wrap */
    if (s->cursor.pending_wrap && s->auto_wrap)
        soft_wrap(s);

    /* Insert mode shifts cells right by the character width */
    if (s->insert_mode && s->cursor.x + (int16_t)width < (int16_t)s->cols)
    {
        ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
        int16_t right = s->margin_region.right;
        int16_t shift = width;
        if (s->cursor.x + shift > right + 1)
            shift = right - s->cursor.x + 1;
        if (shift > 0)
        {
            memmove(&r->cells[s->cursor.x + shift], &r->cells[s->cursor.x],
                    (size_t)(right - s->cursor.x + 1 - shift) * sizeof(ghostcon_cell_t));
            for (int16_t i = 0; i < shift; i++)
                r->cells[s->cursor.x + i] = GC_CELL_EMPTY;
            mark_dirty(s, s->cursor.y);
        }
    }

    switch (width)
    {
    case 1:
        print_cell(s, codepoint, GC_CELL_WIDE_NARROW);
        s->last_codepoint = codepoint;
        break;
    /* Wide characters require a spacer: the first cell is flagged wide and
     * holds the character, the second is a spacer tail.
     */
    case 2:
        if ((right_limit - s->margin_region.left) > 1)
        {
            if (s->cursor.x == right_limit - 1)
            {
                /* Not enough room — insert a spacer and wrap first. */
                if (!s->auto_wrap) return;
                if (right_limit == (int16_t)s->cols)
                    print_cell(s, 0, GC_CELL_WIDE_SPACER_HEAD);
                else
                    print_cell(s, 0, GC_CELL_WIDE_NARROW);
                soft_wrap(s);
            }
            print_cell(s, codepoint, GC_CELL_WIDE_WIDE);
            s->cursor.x++;
            if (s->cursor.x >= (int16_t)s->cols)
                s->cursor.x = (int16_t)(s->cols - 1);
            print_cell(s, 0, GC_CELL_WIDE_SPACER_TAIL);
        }
        else
        {
            print_cell(s, 0, GC_CELL_WIDE_NARROW);
        }
        s->last_codepoint = codepoint;
        break;

    default:
        return;
    }

    /* If we're at the column limit, set pending wrap
     *
     * the wrap itself is deferred until the next character (matches Ghostty).
     */
    if (s->cursor.x == right_limit - 1)
    {
        s->cursor.pending_wrap = true;
        return;
    }

    s->cursor.x++;
}

void ghostcon_screen_put_text(ghostcon_screen_t *s, const uint32_t *codepoints, size_t len)
{
    for (size_t i = 0; i < len; i++)
        ghostcon_screen_put_char(s, codepoints[i]);
}


/* Erase operations */

void ghostcon_screen_erase_display(ghostcon_screen_t *s, int mode)
{
    int16_t top, bottom;

    switch (mode)
    {
    case GC_ERASE_DISPLAY_BELOW:
        top = s->cursor.y;
        bottom = (int16_t)(s->rows_visible - 1);
        break;
    case GC_ERASE_DISPLAY_ABOVE:
        top = 0;
        bottom = s->cursor.y;
        break;
    case GC_ERASE_DISPLAY_ALL:
        top = 0;
        bottom = (int16_t)(s->rows_visible - 1);
        /* Matches Ghostty's own eraseDisplay(): .complete clears all Kitty
         * graphics state, not just cells.
         *
         * Images/placements live in a side table decoupled from the cell grid by
         * design (see kitty_graphics.h), so without this an ED (e.g.
         *
         * `clear`, or any TUI's own "wipe the screen" ESC[2J) leaves every
         * previously displayed image rendering forever underneath whatever gets
         * drawn next
         *
         * confirmed live: a page's own image was still visible under an unrelated
         * later page's content, while the same sequence on real Ghostty correctly
         * cleared it.
         *
         * Only .complete/.scroll_complete do this in Ghostty, not the partial
         * .above/.below modes
         *
         * and Ghostty does it unconditionally regardless of DECSED protection, so
         * ghostcon_screen_erase_display_protected's GC_ERASE_DISPLAY_ALL case
         * below gets the identical treatment.
         *
         * Only the ACTIVE screen's images
         *
         * matching Ghostty, which erases screens.active, not both
         *
         * so an ED on the primary screen doesn't reach through and wipe images
         * placed on the alt screen (or vice versa.
         *
         * See kitty_graphics_alt's own doc comment on why these are separate
         * instances at all).
         */
        {
            ghostcon_kitty_graphics_t *akg = ghostcon_screen_active_kitty_graphics(s);
            ghostcon_kitty_graphics_deinit(akg);
            ghostcon_kitty_graphics_init(akg);
            ghostcon_sixel_state_t *asx = ghostcon_screen_active_sixel_state(s);
            ghostcon_sixel_state_deinit(asx);
            ghostcon_sixel_state_init(asx);
        }
        break;
    case GC_ERASE_DISPLAY_SCROLLBACK:
        /* Clear scrollback history */
        if (s->history)
        {
            for (uint16_t i = 0; i < s->history_count; i++)
                ghostcon_row_clear(&s->history[i]);
            s->history_count = 0;
        }
        /* Nothing left to be scrolled back into
         *
         * e.g. clear_on_logout wiping scrollback on session death while the
         * outgoing user happened to be scrolled up would otherwise leave
         * view_offset pointing past the now-empty history (harmless,
         * ghostcon_screen_row() clamps defensively, but the new session should
         * start at live regardless).
         */
        if (s->view_offset > 0)
        {
            s->view_offset = 0;
            s->dirty.y_min = 0;
            s->dirty.y_max = (int16_t)(s->rows_visible - 1);
        }
        return;
    case GC_ERASE_DISPLAY_SCROLL_COMPLETE:
        /* Scroll entire display into scrollback. push_history() steals r->cells
         * by struct-copying it into the history slot
         *
         * it does NOT null out r->cells on the source, so the caller must give r
         * a fresh cells buffer of its own afterward.
         *
         * Fuzzer-found double-free: the previous version allocated that fresh row
         * via ghostcon_row_init() only to check `.cells` was non-NULL, then
         * discarded it
         *
         * r kept pointing at the exact buffer that had just been stolen into
         * history, so s->rows[] and s->history[] both freed the same allocation
         * on cleanup.
         */
        for (int16_t i = 0; i < (int16_t)s->rows_visible; i++)
        {
            ghostcon_row_t *r = &s->rows[row_idx(s, i)];
            ghostcon_row_t fresh = ghostcon_row_init(s->cols);
            if (fresh.cells)
            {
                push_history(s, r);
                *r = fresh;
            }
            else
            {
                ghostcon_row_clear(r);
            }
            mark_dirty(s, i);
        }
        {
            ghostcon_kitty_graphics_t *akg = ghostcon_screen_active_kitty_graphics(s);
            ghostcon_kitty_graphics_deinit(akg);
            ghostcon_kitty_graphics_init(akg);
            ghostcon_sixel_state_t *asx = ghostcon_screen_active_sixel_state(s);
            ghostcon_sixel_state_deinit(asx);
            ghostcon_sixel_state_init(asx);
        }
        return;
    default:
        return;
    }

    ghostcon_cell_t fill = erase_fill_cell(s);
    for (int16_t y = top; y <= bottom; y++)
    {
        ghostcon_row_t *r = &s->rows[row_idx(s, y)];
        if (y == s->cursor.y && mode != GC_ERASE_DISPLAY_ALL)
        {
            /* Erase from cursor x to end of line */
            uint16_t left = (mode == GC_ERASE_DISPLAY_ABOVE) ? 0 : s->cursor.x;
            uint16_t end  = (mode == GC_ERASE_DISPLAY_ABOVE) ? (uint16_t)(s->cursor.x + 1) : s->cols;
            ghostcon_row_fill_range(r, left, end, fill);
            /* Ghostty's ED.below calls eraseLine(.right) which resets the
             * cursor row's soft-wrap; ED.above calls eraseLine(.left) which
             * preserves it.
             */
            if (mode == GC_ERASE_DISPLAY_BELOW)
            {
                r->wrap = false;
                r->wrap_continuation = false;
            }
        }
        else
        {
            ghostcon_row_fill(r, fill);
        }
        mark_dirty(s, y);
    }
}

void ghostcon_screen_erase_display_protected(ghostcon_screen_t *s, int mode)
{
    int16_t top, bottom;

    switch (mode)
    {
    case GC_ERASE_DISPLAY_BELOW:
        top = s->cursor.y;
        bottom = (int16_t)(s->rows_visible - 1);
        break;
    case GC_ERASE_DISPLAY_ABOVE:
        top = 0;
        bottom = s->cursor.y;
        break;
    case GC_ERASE_DISPLAY_ALL:
        top = 0;
        bottom = (int16_t)(s->rows_visible - 1);
        /* Same as the unprotected ghostcon_screen_erase_display's
         * GC_ERASE_DISPLAY_ALL case above
         *
         * Ghostty clears all Kitty graphics state for a full-screen erase
         * unconditionally, regardless of DECSED protection.
         *
         * Active screen only, same reasoning as that case too.
         */
        {
            ghostcon_kitty_graphics_t *akg = ghostcon_screen_active_kitty_graphics(s);
            ghostcon_kitty_graphics_deinit(akg);
            ghostcon_kitty_graphics_init(akg);
            ghostcon_sixel_state_t *asx = ghostcon_screen_active_sixel_state(s);
            ghostcon_sixel_state_deinit(asx);
            ghostcon_sixel_state_init(asx);
        }
        break;
    case GC_ERASE_DISPLAY_SCROLLBACK:
    case GC_ERASE_DISPLAY_SCROLL_COMPLETE:
        /* Scrollback erasure ignores protection */
        ghostcon_screen_erase_display(s, mode);
        return;
    default:
        return;
    }

    /* DECSED: like ED but skips cells with the protected bit set. */
    ghostcon_cell_t fill = erase_fill_cell(s);
    for (int16_t y = top; y <= bottom; y++)
    {
        ghostcon_row_t *r = &s->rows[row_idx(s, y)];
        if (y == s->cursor.y && mode != GC_ERASE_DISPLAY_ALL)
        {
            uint16_t left = (mode == GC_ERASE_DISPLAY_ABOVE) ? 0 : s->cursor.x;
            uint16_t end  = (mode == GC_ERASE_DISPLAY_ABOVE) ? (uint16_t)(s->cursor.x + 1) : s->cols;
            ghostcon_row_fill_range_unprotected(r, left, end, fill);
        }
        else
        {
            ghostcon_row_fill_range_unprotected(r, 0, s->cols, fill);
        }
        mark_dirty(s, y);
    }
}

void ghostcon_screen_erase_line(ghostcon_screen_t *s, int mode)
{
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t left, end;

    switch (mode)
    {
    case GC_ERASE_LINE_RIGHT:
        left = s->cursor.x;
        end = s->cols;
        break;
    case GC_ERASE_LINE_LEFT:
        left = 0;
        end = s->cursor.x + 1;
        break;
    case GC_ERASE_LINE_ALL:
        left = 0;
        end = s->cols;
        break;
    default:
        return;
    }

    ghostcon_row_fill_range(r, left, end, erase_fill_cell(s));
    /* Ghostty's eraseLine(.right) resets the row's soft-wrap state
     * (cursorResetWrap); .left and .complete preserve it.
     */
    if (mode == GC_ERASE_LINE_RIGHT)
    {
        r->wrap = false;
        r->wrap_continuation = false;
    }
    mark_dirty(s, s->cursor.y);
}

void ghostcon_screen_erase_line_protected(ghostcon_screen_t *s, int mode)
{
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t left, end;

    switch (mode)
    {
    case GC_ERASE_LINE_RIGHT:
        left = s->cursor.x;
        end = s->cols;
        break;
    case GC_ERASE_LINE_LEFT:
        left = 0;
        end = s->cursor.x + 1;
        break;
    case GC_ERASE_LINE_ALL:
        left = 0;
        end = s->cols;
        break;
    default:
        return;
    }

    /* DECSEL: like EL but skips cells with the protected bit set. */
    ghostcon_row_fill_range_unprotected(r, left, end, erase_fill_cell(s));
    mark_dirty(s, s->cursor.y);
}

void ghostcon_screen_save_mode(ghostcon_screen_t *s, int mode)
{
    switch (mode)
    {
    case 1:  s->saved_modes.application_cursor  = s->application_cursor; break; /* DECCKM */
    case 4:  s->saved_modes.insert_mode         = s->insert_mode; break;        /* IRM */
    case 5:  s->saved_modes.reverse_video       = s->reverse_video; break;      /* DECSCNM */
    case 6:  s->saved_modes.origin_mode         = s->origin_mode; break;        /* DECOM */
    case 7:  s->saved_modes.auto_wrap           = s->auto_wrap; break;          /* DECAWM */
    case 69: s->saved_modes.left_right_margin   = s->left_right_margin; break;  /* DECLRMM */
    default: break;
    }
}

void ghostcon_screen_restore_mode(ghostcon_screen_t *s, int mode)
{
    switch (mode)
    {
    case 1:  s->application_cursor = s->saved_modes.application_cursor; break;
    case 4:  s->insert_mode        = s->saved_modes.insert_mode; break;
    case 5:  s->reverse_video      = s->saved_modes.reverse_video; break;
    case 6:  s->origin_mode        = s->saved_modes.origin_mode; break;
    case 7:  s->auto_wrap          = s->saved_modes.auto_wrap; break;
    case 69: s->left_right_margin  = s->saved_modes.left_right_margin; break;
    default: break;
    }
}

void ghostcon_screen_erase_chars(ghostcon_screen_t *s, uint16_t n)
{
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t end = s->cursor.x + n;
    if (end > s->cols) end = s->cols;
    ghostcon_row_fill_range(r, s->cursor.x, end, erase_fill_cell(s));
    mark_dirty(s, s->cursor.y);
}

void ghostcon_screen_insert_chars(ghostcon_screen_t *s, uint16_t n)
{
    if (n == 0) return;
    int16_t right = s->margin_region.right;

    /* The cursor can legitimately sit past the margin region entirely --absolute
     * CUP is NOT constrained to margin_region.right (see
     * ghostcon_screen_cursor_set()'s own clamp, which only bounds against cols-1)
     *
     * so ICH issued from out there is a no-op, not "insert some huge count of
     * chars".
     *
     * Explicit signed guard, not clamp-and-hope: an earlier version computed the
     * clamped count through uint16_t arithmetic that silently wrapped to ~65000
     * on exactly this precondition, and only stayed harmless because a later
     * round-trip cast happened to cancel the wrapped value back out
     *
     * correct by algebraic coincidence, not by inspection, and not something a
     * future edit could be trusted to preserve.
     */
    if (s->cursor.x > right)
        return;

    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    /* available > 0 is guaranteed by the guard above; int32_t throughout so
     * nothing here can overflow int16_t/wrap an unsigned type regardless of
     * how cursor.x/right/n relate.
     */
    int32_t available = (int32_t)right - (int32_t)s->cursor.x + 1;
    int32_t shift = n;
    if (shift > available)
        shift = available;

    memmove(&r->cells[s->cursor.x + shift], &r->cells[s->cursor.x],
            (size_t)(available - shift) * sizeof(ghostcon_cell_t));

    for (int32_t i = s->cursor.x; i < s->cursor.x + shift; i++)
        r->cells[i] = GC_CELL_EMPTY;

    mark_dirty(s, s->cursor.y);
}

void ghostcon_screen_delete_chars(ghostcon_screen_t *s, uint16_t n)
{
    if (n == 0) return;
    int16_t right = s->margin_region.right;

    /* Same reasoning as ghostcon_screen_insert_chars() right above: the cursor
     * can legitimately sit past the margin region entirely (absolute CUP isn't
     * constrained to it), and DCH issued from out there is a no-op. int32_t
     * throughout, and an explicit guard for that precondition, rather than
     * relying on an implementation-defined narrowing conversion (assigning `right
     *
     * - n + 1` -- possibly far outside int16_t's range when n is large -- into an
     *   int16_t loop variable) to wrap around to something that happens to make
     *   the loop bound work out, the way an earlier version implicitly did.
     */
    if (s->cursor.x > right)
        return;

    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    int32_t available = (int32_t)right - (int32_t)s->cursor.x + 1; /* > 0, guaranteed above */
    int32_t count_to_delete = n;
    if (count_to_delete > available)
        count_to_delete = available;

    int32_t src = s->cursor.x + count_to_delete;
    int32_t remaining = available - count_to_delete;
    if (remaining > 0)
        memmove(&r->cells[s->cursor.x], &r->cells[src], (size_t)remaining * sizeof(ghostcon_cell_t));

    for (int32_t i = s->cursor.x + remaining; i <= right; i++)
        r->cells[i] = GC_CELL_EMPTY;

    mark_dirty(s, s->cursor.y);
}

/* Insert/Delete lines */

void ghostcon_screen_insert_lines(ghostcon_screen_t *s, uint16_t n)
{
    if (n == 0) return;
    int32_t top = s->cursor.y;
    int32_t bottom = s->scroll_region.bottom;
    if (top > bottom)
        return;

    /* n is attacker-controlled (up to UINT16_MAX) and unrelated to the scroll
     * region's actual height
     *
     * clamp before any arithmetic so a huge n can't wrap through int16_t and
     * corrupt the loop bounds (see the identical class of bug fixed in
     * insert_chars/delete_chars).
     */
    int32_t available = bottom - top + 1;
    int32_t shift = n;
    if (shift > available)
        shift = available;

    /* Scroll lines [top, bottom-shift] down by shift */
    for (int32_t y = bottom; y >= top + shift; y--)
    {
        ghostcon_row_t *dst = &s->rows[row_idx(s, (int16_t)y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, (int16_t)(y - shift))];
        /* Copy cells */
        ghostcon_row_copy(dst, src);
        mark_dirty(s, (int16_t)y);
    }

    /* Clear the top shift lines */
    ghostcon_cell_t fill = erase_fill_cell(s);
    for (int32_t y = top; y < top + shift && y <= bottom; y++)
    {
        ghostcon_row_fill(&s->rows[row_idx(s, (int16_t)y)], fill);
        mark_dirty(s, (int16_t)y);
    }
}

void ghostcon_screen_delete_lines(ghostcon_screen_t *s, uint16_t n)
{
    if (n == 0) return;
    int32_t top = s->cursor.y;
    int32_t bottom = s->scroll_region.bottom;
    if (top > bottom)
        return;

    int32_t available = bottom - top + 1;
    int32_t count_to_delete = n;
    if (count_to_delete > available)
        count_to_delete = available;

    /* Scroll lines [top+count_to_delete, bottom] up by count_to_delete */
    for (int32_t y = top; y <= bottom - count_to_delete; y++)
    {
        ghostcon_row_t *dst = &s->rows[row_idx(s, (int16_t)y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, (int16_t)(y + count_to_delete))];
        ghostcon_row_copy(dst, src);
        mark_dirty(s, (int16_t)y);
    }

    /* Clear the bottom count_to_delete lines */
    for (int32_t y = bottom - count_to_delete + 1; y <= bottom; y++)
    {
        ghostcon_row_clear(&s->rows[row_idx(s, (int16_t)y)]);
        mark_dirty(s, (int16_t)y);
    }
}
