#include "ghostcon/term/screen.h"
#include "screen_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Lifecycle */

bool ghostcon_screen_init(ghostcon_screen_t *s, uint16_t cols, uint16_t rows, uint16_t scrollback_cap)
{
    memset(s, 0, sizeof(*s));

    s->cols = cols;
    s->rows_visible = rows;
    s->scrollback_top = 0;

    /* Allocate visible grid (ring buffer) */
    s->rows = (ghostcon_row_t *)calloc(rows, sizeof(ghostcon_row_t));
    if (!s->rows) goto fail;

    for (uint16_t i = 0; i < rows; i++)
    {
        s->rows[i] = ghostcon_row_init(cols);
        if (!s->rows[i].cells && cols > 0) goto fail;
    }

    /* Allocate history ring buffer */
    if (scrollback_cap > 0)
    {
        s->history_cap = scrollback_cap;
        s->history = (ghostcon_row_t *)calloc(scrollback_cap, sizeof(ghostcon_row_t));
        if (!s->history) goto fail;

        for (uint16_t i = 0; i < scrollback_cap; i++)
        {
            s->history[i] = ghostcon_row_init(cols);
            if (!s->history[i].cells && cols > 0) goto fail;
        }
    }

    /* Allocate style set */
    s->styles = ghostcon_style_set_create(64);
    if (!s->styles) goto fail;

    /* Allocate hyperlink set (OSC 8) */
    s->hyperlinks = ghostcon_hyperlink_set_create(16);
    if (!s->hyperlinks) goto fail;

    /* Tab stops — every 8 columns by default */
    s->tabstops.cols = cols;
    s->tabstops.stops = (bool *)calloc(cols, sizeof(bool));
    if (!s->tabstops.stops) goto fail;
    for (uint16_t i = 8; i < cols; i += 8)
    {
        s->tabstops.stops[i] = true;
    }

    /* Scroll region defaults to full screen */
    s->scroll_region.top = 0;
    s->scroll_region.bottom = (int16_t)(rows - 1);

    /* Margin region defaults to full width */
    s->margin_region.left = 0;
    s->margin_region.right = (int16_t)(cols - 1);

    /* Default mode flags */
    s->auto_wrap = true;
    s->cursor_visible = true; /* DECTCEM -- unlike every other private
        mode, this one defaults ON on a real terminal; nothing sends
        CSI ?25h at startup since it's assumed already visible, so
        leaving this at the struct's memset-zero default (like every
        other mode flag) would make the cursor invisible from the very
        first frame -- confirmed live: this was exactly the reported
        "no cursor on the login prompt" bug, not (only) a missing
        render call. */

    /* Cursor at home */
    s->cursor.x = 0;
    s->cursor.y = 0;
    s->cursor.style_id = GC_STYLE_DEFAULT_ID;

    /* Shell integration: no command has finished yet */
    s->semantic_last_exit_code = -1;

    /* Dirty state: everything, not nothing
     *
     * a fresh screen still needs its first frame painted (uniform background at
     * minimum), same as the full-reset convention below (ghostcon_screen_reset).
     *
     * Leaving this -1/-1 meant any row never explicitly written to (common
     *
     * most sessions don't fill every row) never got a background quad pushed at
     * all on the first frame, showing whatever was already in the render target
     * instead of the terminal's actual background
     *
     * found via real hardware testing: "rows that aren't allocated by chars have
     * different background than the ones that are allocated".
     */
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(rows - 1);

    /* Palette */
    ghostcon_palette_init(&s->palette);

    /* Selection */
    ghostcon_selection_clear(&s->selection);

    /* Kitty keyboard protocol */
    ghostcon_kitty_init(&s->kitty);

    /* Kitty graphics protocol (images + placements)
     *
     * one instance per screen, see screen.h's doc comment on kitty_graphics_alt.
     */
    ghostcon_kitty_graphics_init(&s->kitty_graphics);
    ghostcon_kitty_graphics_init(&s->kitty_graphics_alt);

    /* Sixel graphics state
     *
     * one instance per screen, same reasoning as
     * kitty_graphics/kitty_graphics_alt above.
     */
    ghostcon_sixel_state_init(&s->sixel);
    ghostcon_sixel_state_init(&s->sixel_alt);

    /* Mode bitfield */
    ghostcon_modes_clear(&s->modes);
    if (s->auto_wrap) ghostcon_modes_set(&s->modes, GC_MODE_AUTO_WRAP);

    return true;

fail:
    ghostcon_screen_deinit(s);
    return false;
}

void ghostcon_screen_deinit(ghostcon_screen_t *s)
{
    if (s->rows)
    {
        for (uint16_t i = 0; i < s->rows_visible; i++)
            ghostcon_row_deinit(&s->rows[i]);
        free(s->rows);
    }
    if (s->history)
    {
        for (uint16_t i = 0; i < s->history_cap; i++)
            ghostcon_row_deinit(&s->history[i]);
        free(s->history);
    }
    if (s->alt_rows)
    {
        for (uint16_t i = 0; i < s->alt_rows_visible; i++)
            ghostcon_row_deinit(&s->alt_rows[i]);
        free(s->alt_rows);
    }
    ghostcon_style_set_destroy(s->styles);
    ghostcon_hyperlink_set_destroy(s->hyperlinks);
    free(s->tabstops.stops);
    ghostcon_kitty_graphics_deinit(&s->kitty_graphics);
    ghostcon_kitty_graphics_deinit(&s->kitty_graphics_alt);
    ghostcon_sixel_state_deinit(&s->sixel);
    ghostcon_sixel_state_deinit(&s->sixel_alt);
    memset(s, 0, sizeof(*s));
}

ghostcon_kitty_graphics_t
*ghostcon_screen_active_kitty_graphics(ghostcon_screen_t *s)
{
    return s->alt_screen_active ? &s->kitty_graphics_alt : &s->kitty_graphics;
}

ghostcon_sixel_state_t
*ghostcon_screen_active_sixel_state(ghostcon_screen_t *s)
{
    return s->alt_screen_active ? &s->sixel_alt : &s->sixel;
}

bool ghostcon_screen_resize(ghostcon_screen_t *s, uint16_t new_cols, uint16_t new_rows)
{
    /* Row layout (both rows_visible's ring geometry and every row's width) is
     * about to change
     *
     * a view_offset computed against the old layout would splice in the wrong
     * lines.
     *
     * Simplest correct behavior: snap back to live, matching how a resize already
     * disrupts scroll position in most real terminals.
     */
    s->view_offset = 0;

    /* TODO: proper reflow — for now, simple truncate/extend */
    if (new_cols != s->cols)
    {
        for (uint16_t i = 0; i < s->rows_visible; i++)
            ghostcon_row_resize(&s->rows[i], new_cols);
        if (s->history)
        {
            for (uint16_t i = 0; i < s->history_cap; i++)
                ghostcon_row_resize(&s->history[i], new_cols);
        }
    }

    if (new_rows != s->rows_visible)
    {
        /* Deinit rows being removed BEFORE realloc */
        if (new_rows < s->rows_visible)
        {
            for (uint16_t i = new_rows; i < s->rows_visible; i++)
                ghostcon_row_deinit(&s->rows[i]);
        }

        ghostcon_row_t *new_grid = (ghostcon_row_t *)realloc(
            s->rows, new_rows * sizeof(ghostcon_row_t));
        if (!new_grid) return false;

        if (new_rows > s->rows_visible)
        {
            for (uint16_t i = s->rows_visible; i < new_rows; i++)
                new_grid[i] = ghostcon_row_init(new_cols);
        }
        s->rows = new_grid;
        s->rows_visible = new_rows;
    }

    s->cols = new_cols;
    s->scroll_region.bottom = (int16_t)(new_rows - 1);
    s->margin_region.right = (int16_t)(new_cols - 1);

    if (s->cursor.x >= (int16_t)new_cols) s->cursor.x = (int16_t)(new_cols - 1);
    if (s->cursor.y >= (int16_t)new_rows) s->cursor.y = (int16_t)(new_rows - 1);

    /* Reallocate tabstops */
    bool *new_stops = (bool *)realloc(s->tabstops.stops, new_cols * sizeof(bool));
    if (!new_stops) return false;
    if (new_cols > s->tabstops.cols)
    {
        memset(&new_stops[s->tabstops.cols], 0,
               (new_cols - s->tabstops.cols) * sizeof(bool));
    }
    s->tabstops.stops = new_stops;
    s->tabstops.cols = new_cols;

    /* Mark all dirty */
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(new_rows - 1);
    for (uint16_t i = 0; i < new_rows; i++)
        s->rows[i].dirty = true;

    return true;
}

void ghostcon_screen_reset(ghostcon_screen_t *s)
{
    /* Drop out of the alternate screen if active
     *
     * RIS is a hard reset, not a graceful exit, so the saved main-screen content
     * is simply discarded rather than restored (see this function's own doc
     * comment in screen.h).
     */
    if (s->alt_screen_active)
    {
        for (uint16_t i = 0; i < s->alt_rows_visible; i++)
            ghostcon_row_deinit(&s->alt_rows[i]);
        free(s->alt_rows);
        s->alt_rows = NULL;
        s->alt_rows_visible = 0;
        s->alt_screen_active = false;
    }

    /* Clear the visible screen
     *
     * scrollback history is left intact, matching xterm's own RIS convention.
     */
    for (uint16_t i = 0; i < s->rows_visible; i++)
        ghostcon_row_clear(&s->rows[i]);
    s->view_offset = 0;

    /* RIS is a hard reset
     *
     * drop all Kitty graphics images/placements on BOTH screens, same "discard
     * rather than gracefully restore" reasoning as the alt-screen drop above.
     *
     * Both, not just whichever was active, since alt_screen_active was just
     * forced false above
     *
     * resetting only "the active one" here would leave the other screen's images
     * never freed.
     */
    ghostcon_kitty_graphics_deinit(&s->kitty_graphics);
    ghostcon_kitty_graphics_init(&s->kitty_graphics);
    ghostcon_kitty_graphics_deinit(&s->kitty_graphics_alt);
    ghostcon_kitty_graphics_init(&s->kitty_graphics_alt);

    /* Same for sixel placements on both screens. */
    ghostcon_sixel_state_deinit(&s->sixel);
    ghostcon_sixel_state_init(&s->sixel);
    ghostcon_sixel_state_deinit(&s->sixel_alt);
    ghostcon_sixel_state_init(&s->sixel_alt);

    /* Cursor */
    s->cursor.x = 0;
    s->cursor.y = 0;
    s->cursor.pending_wrap = false;
    s->cursor.protected = false;
    s->cursor.style_id = GC_STYLE_DEFAULT_ID;
    s->cursor.hyperlink_id = 0;
    s->cursor.cursor_style = GC_CURSOR_DEFAULT;
    memset(&s->saved_cursor, 0, sizeof(s->saved_cursor));
    s->cursor_saved = false;
    s->last_codepoint = 0;

    /* Regions & tabs back to full width/height
     *
     * this is what fixes the DECRST-69-doesn't-reset-margins bug (see the
     * separate fix in stream.c's DECRST handler), and gives RIS a way to recover
     * even if some other bug leaves a region stuck.
     */
    s->scroll_region.top = 0;
    s->scroll_region.bottom = (int16_t)(s->rows_visible - 1);
    s->margin_region.left = 0;
    s->margin_region.right = (int16_t)(s->cols - 1);
    memset(s->tabstops.stops, 0, s->tabstops.cols * sizeof(bool));
    for (uint16_t i = 8; i < s->tabstops.cols; i += 8)
        s->tabstops.stops[i] = true;

    /* Mode flags back to power-on defaults (mirrors ghostcon_screen_init) */
    s->origin_mode = false;
    s->auto_wrap = true;
    s->cursor_visible = true;
    s->reverse_video = false;
    s->insert_mode = false;
    s->application_cursor = false;
    s->synchronized_output = false;
    s->bracketed_paste = false;
    s->left_right_margin = false;
    s->mouse_tracking = false;
    s->mouse_protocol = 0;
    s->mouse_sgr = false;
    s->mouse_shift_capture = false;
    s->protected_mode = GC_PROTECTED_OFF;
    memset(&s->saved_modes, 0, sizeof(s->saved_modes));

    ghostcon_modes_clear(&s->modes);
    if (s->auto_wrap) ghostcon_modes_set(&s->modes, GC_MODE_AUTO_WRAP);

    /* Shell integration (OSC 133/633) */
    s->semantic_current = (ghostcon_cell_semantic_t)0;
    s->semantic_last_exit_code = -1;

    /* Selection and Kitty keyboard protocol state */
    ghostcon_selection_clear(&s->selection);
    ghostcon_kitty_init(&s->kitty);

    /* The whole screen was just cleared
     *
     * mark it all dirty so the next frame actually paints over whatever was there
     * before.
     */
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(s->rows_visible - 1);
}

/* Attribute/style */

ghostcon_style_id_t
ghostcon_screen_set_style(ghostcon_screen_t *s, ghostcon_style_id_t style_id)
{
    s->cursor.style_id = style_id;
    return style_id;
}

/* Apply an SGR attribute
 *
 * this is a thin wrapper that will be expanded when integrated with
 * libghostty-vt's SGR parser. For now, a no-op.
 */
ghostcon_style_t
ghostcon_screen_apply_sgr_attribute(const ghostcon_style_t *base, int sgr_tag, const void *sgr_value)
{
    (void)sgr_tag;
    (void)sgr_value;
    return *base;
}

/* Damage tracking */

void ghostcon_screen_mark_dirty(ghostcon_screen_t *s, uint16_t y)
{
    s->rows[row_idx(s, (int16_t)y)].dirty = true;
    if (s->dirty.y_min < 0 || (int16_t)y < s->dirty.y_min)
        s->dirty.y_min = (int16_t)y;
    if (s->dirty.y_max < 0 || (int16_t)y > s->dirty.y_max)
        s->dirty.y_max = (int16_t)y;
}

void ghostcon_screen_clear_dirty(ghostcon_screen_t *s)
{
    s->dirty.y_min = -1;
    s->dirty.y_max = -1;
}

ghostcon_dirty_region_t
ghostcon_screen_get_dirty(const ghostcon_screen_t *s)
{
    return s->dirty;
}

/* Direct cell access */

ghostcon_cell_t
*ghostcon_screen_cell(ghostcon_screen_t *s, uint16_t x, uint16_t y)
{
    if (y >= s->rows_visible || x >= s->cols)
        return NULL;
    return &s->rows[row_idx(s, (int16_t)y)].cells[x];
}

ghostcon_row_t
*ghostcon_screen_row(ghostcon_screen_t *s, uint16_t y)
{
    if (y >= s->rows_visible)
        return NULL;

    if (s->view_offset > 0)
    {
        /* Splice history in above the live grid. Chronological order,
         * oldest to newest: history[0..history_count) then
         * rows[0..rows_visible).
         *
         * Scrolled back by `view_offset` lines, viewport row y shows
         * chronological position (history_count -view_offset + y).
         */
        uint16_t offset = s->view_offset;
        if (offset > s->history_count)
            offset = s->history_count; /* defensive; scroll_view() already clamps */

        long logical = (long)s->history_count - (long)offset + (long)y;
        if (logical < (long)s->history_count)
        {
            /* history_head is the ring buffer's next-write slot, so the
             * oldest live entry (chronological index 0) sits at
             * history_head - history_count, wrapping mod history_cap.
             */
            long slot = (long)s->history_head - (long)s->history_count + logical;
            slot = ((slot % s->history_cap) + s->history_cap) % s->history_cap;
            return &s->history[slot];
        }
        int16_t live_y = (int16_t)(logical - (long)s->history_count);
        return &s->rows[row_idx(s, live_y)];
    }

    return &s->rows[row_idx(s, (int16_t)y)];
}

void ghostcon_screen_scroll_view(ghostcon_screen_t *s, int delta)
{
    int new_offset = (int)s->view_offset + delta;
    if (new_offset < 0)
        new_offset = 0;
    if (new_offset > (int)s->history_count)
        new_offset = (int)s->history_count;

    if ((uint16_t)new_offset == s->view_offset)
        return;

    s->view_offset = (uint16_t)new_offset;

    /* The whole viewport's content just changed (scrolled), not any one line
     *
     * mark everything dirty rather than diffing old vs new per-row.
     */
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(s->rows_visible - 1);
}

bool ghostcon_screen_sync_output_expired(const ghostcon_screen_t *s, const struct timespec *now, double timeout_ms)
{
    if (!s || !s->synchronized_output || !now)
        return true;

    double elapsed_ms =
        (now->tv_sec - s->synchronized_output_since.tv_sec) * 1000.0 +
        (now->tv_nsec - s->synchronized_output_since.tv_nsec) / 1e6;
    return elapsed_ms >= timeout_ms;
}

