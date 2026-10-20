#include "ghostcon/term/screen.h"
#include "ghostcon/term/screen_scroll.h"
#include "screen_internal.h"
#include <stdlib.h>
#include <string.h>

/* Scrolling */

void ghostcon_screen_scroll_up(ghostcon_screen_t *s, uint16_t n)
{
    int16_t top = s->scroll_region.top;
    int16_t bottom = s->scroll_region.bottom;

    if (top > bottom)
        return;

    int16_t region_height = bottom - top + 1;
    if (n > (uint16_t)region_height)
        n = region_height;

    for (uint16_t i = 0; i < n; i++)
    {
        /* Push the top line of the scroll region into history */
        ghostcon_row_t *top_row = &s->rows[row_idx(s, top + i)];

        /* Create a copy to push into history */
        ghostcon_row_t hist_copy = ghostcon_row_init(s->cols);
        if (hist_copy.cells)
        {
            memcpy(hist_copy.cells, top_row->cells, s->cols * sizeof(ghostcon_cell_t));
            hist_copy.wrap = top_row->wrap;
            hist_copy.wrap_continuation = top_row->wrap_continuation;
            push_history(s, &hist_copy);
        }
        else
        {
            /* If allocation fails, just lose the history */
        }
    }

    /* Shift rows down (bottom row gets cleared) */
    for (int16_t y = top; y <= bottom - (int16_t)n; y++)
    {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y + n)];
        ghostcon_row_copy(dst, src);
        mark_dirty(s, y);
    }

    /* Clear the bottom n lines of the scroll region */
    ghostcon_cell_t fill = erase_fill_cell(s);
    for (int16_t y = bottom - (int16_t)n + 1; y <= bottom; y++)
    {
        ghostcon_row_fill(&s->rows[row_idx(s, y)], fill);
        mark_dirty(s, y);
    }
}

void ghostcon_screen_scroll_down(ghostcon_screen_t *s, uint16_t n)
{
    int16_t top = s->scroll_region.top;
    int16_t bottom = s->scroll_region.bottom;

    if (top > bottom)
        return;

    int16_t region_height = bottom - top + 1;
    if (n > (uint16_t)region_height)
        n = region_height;

    /* Shift rows up (top line gets cleared) */
    for (int16_t y = bottom; y >= top + (int16_t)n; y--)
    {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y - n)];
        ghostcon_row_copy(dst, src);
        mark_dirty(s, y);
    }

    /* Clear the top n lines of the scroll region */
    ghostcon_cell_t fill = erase_fill_cell(s);
    for (int16_t y = top; y < top + (int16_t)n && y <= bottom; y++)
    {
        ghostcon_row_fill(&s->rows[row_idx(s, y)], fill);
        mark_dirty(s, y);
    }
}

void ghostcon_screen_set_scroll_region(ghostcon_screen_t *s, int16_t top, int16_t bottom)
{
    if (top < 0 || bottom < 0)
    {
        /* Reset to full screen */
        s->scroll_region.top = 0;
        s->scroll_region.bottom = (int16_t)(s->rows_visible - 1);
    }
    else
    {
        /* top/bottom come straight from a raw CSI param (only negative via an
         * int16_t wrap from a huge value, caught above)
         *
         * a mid-range value like 30000 on a ~50-row screen stays positive and
         * would otherwise land here unclamped.
         *
         * Every consumer of scroll_region.top/bottom assumes it's a real row
         * range: row_idx()'s modulo keeps individual accesses in bounds
         * regardless, but region_height = bottom - top + 1 feeds straight into
         * scroll_up/down's per-line history-push loop (a real malloc per
         * iteration) and IL/DL's row-copy loops
         *
         * an unclamped region_height turns one escape sequence into an unbounded
         * allocation storm.
         *
         * Clamp to the actual screen before storing.
         */
        int16_t max_row = (int16_t)(s->rows_visible - 1);
        s->scroll_region.top = top > max_row ? max_row : top;
        s->scroll_region.bottom = bottom > max_row ? max_row : bottom;
    }

    /* Cursor goes to home position: region home if origin mode, otherwise
     * screen home (matches Ghostty setCursorPos(1,1)).
     */
    if (s->origin_mode)
    {
        s->cursor.x = s->margin_region.left;
        s->cursor.y = s->scroll_region.top;
    }
    else
    {
        s->cursor.x = 0;
        s->cursor.y = 0;
    }
    s->cursor.pending_wrap = false;
}

void ghostcon_screen_set_margin_region(ghostcon_screen_t *s, int16_t left, int16_t right)
{
    if (left < 0 || right < 0)
    {
        s->margin_region.left = 0;
        s->margin_region.right = (int16_t)(s->cols - 1);
    }
    else
    {
        /* Same class of gap as set_scroll_region above
         *
         * clamp against the actual column count.
         */
        int16_t max_col = (int16_t)(s->cols - 1);
        s->margin_region.left = left > max_col ? max_col : left;
        s->margin_region.right = right > max_col ? max_col : right;
    }
}

/* Alternate screen */

void ghostcon_screen_alt_screen_enter(ghostcon_screen_t *s)
{
    if (s->alt_screen_active)
        return;

    /* Save main screen grid */
    s->alt_rows_visible = s->rows_visible;
    s->alt_rows = (ghostcon_row_t *)calloc(s->rows_visible, sizeof(ghostcon_row_t));
    if (!s->alt_rows) return;

    for (uint16_t i = 0; i < s->rows_visible; i++)
    {
        s->alt_rows[i] = ghostcon_row_init(s->cols);
        if (s->alt_rows[i].cells && s->rows[i].cells)
        {
            memcpy(s->alt_rows[i].cells, s->rows[i].cells,
                   s->cols * sizeof(ghostcon_cell_t));
            s->alt_rows[i].wrap = s->rows[i].wrap;
            s->alt_rows[i].grapheme = s->rows[i].grapheme;
            s->alt_rows[i].styled = s->rows[i].styled;
        }
    }

    s->alt_scrollback_top = s->scrollback_top;
    s->alt_screen_active = true;

    /* Clear main screen */
    for (uint16_t i = 0; i < s->rows_visible; i++)
    {
        ghostcon_row_clear(&s->rows[i]);
        s->rows[i].dirty = true;
    }
    s->scrollback_top = 0;
    s->cursor.x = 0;
    s->cursor.y = 0;
    s->cursor.pending_wrap = false;
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(s->rows_visible - 1);
}

void ghostcon_screen_alt_screen_exit(ghostcon_screen_t *s)
{
    if (!s->alt_screen_active)
        return;

    /* Restore main screen grid */
    for (uint16_t i = 0; i < s->rows_visible && i < s->alt_rows_visible; i++)
    {
        if (s->rows[i].cells && s->alt_rows[i].cells)
        {
            memcpy(s->rows[i].cells, s->alt_rows[i].cells,
                   s->cols * sizeof(ghostcon_cell_t));
            s->rows[i].wrap = s->alt_rows[i].wrap;
            s->rows[i].grapheme = s->alt_rows[i].grapheme;
            s->rows[i].styled = s->alt_rows[i].styled;
            s->rows[i].dirty = true;
        }
    }

    s->scrollback_top = s->alt_scrollback_top;
    s->cursor.x = 0;
    s->cursor.y = 0;
    s->cursor.pending_wrap = false;

    /* Free saved alt screen */
    for (uint16_t i = 0; i < s->alt_rows_visible; i++)
        ghostcon_row_deinit(&s->alt_rows[i]);
    free(s->alt_rows);
    s->alt_rows = NULL;
    s->alt_screen_active = false;

    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(s->rows_visible - 1);
}
