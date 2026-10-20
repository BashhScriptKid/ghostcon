/* screen_internal.h
 *
 * The screen module's private helpers, shared by screen.c and its three
 * sibling modules (screen_cursor.c, screen_edit.c, screen_scroll.c).
 *
 * NOT part of the public API (it lives under src/, not include/)
 *
 * The screen struct itself is exposed in full (include/ghostcon/term/
 * screen.h), because seven consumers reach into it directly and hiding it
 * would mean rewriting all of them to go through accessors.
 */

#pragma once

#include "ghostcon/term/screen.h"

#include <stdlib.h>
#include <string.h>

/* Internal helpers */

/* Convert a 0-based logical row index to the actual index in the ring buffer */
static inline uint16_t row_idx(ghostcon_screen_t *s, int16_t y)
{
    return (uint16_t)((s->scrollback_top + y) % s->rows_visible);
}

/* Get cell at (x, y) in logical coordinates */
static inline ghostcon_cell_t
*cell_at(ghostcon_screen_t *s, int16_t x, int16_t y)
{
    return &s->rows[row_idx(s, y)].cells[x];
}

/* Build the blank "erase color" cell used to fill cells vacated by ED/EL/ECH and
 * by scrolling: per ECMA-48, erased cells take on the *currently selected*
 * background color (the SGR pen's bg), not the terminal default.
 *
 * Matches Ghostty's own representation: a blank cell carrying only a background
 * gets tagged BG_COLOR_PALETTE/BG_COLOR_RGB in its content field, style_id left
 * at 0 (default)
 *
 * not a style-table entry.
 *
 * This avoids interning a near-duplicate default-ish style per distinct erase
 * color, and keeps large erased regions cheap.
 */
static inline ghostcon_cell_t
erase_fill_cell(ghostcon_screen_t *s)
{
    const ghostcon_style_t *pen = ghostcon_style_set_get(s->styles, s->cursor.style_id);

    ghostcon_cell_t fill = {0};
    if (pen->flags & GC_STYLE_BG_DEFAULT)
        return fill; /* no explicit bg -- plain empty/default cell */

    if (pen->flags & GC_STYLE_BG_TRUECOLOR)
    {
        ghostcon_cell_set_tag(&fill, GC_CELL_BG_COLOR_RGB);
        ghostcon_cell_set_rgb(&fill, pen->bg_rgb);
    }
    else
    {
        ghostcon_cell_set_tag(&fill, GC_CELL_BG_COLOR_PALETTE);
        ghostcon_cell_set_palette_idx(&fill, pen->bg_palette);
    }
    return fill;
}

/* Mark a cell's row as dirty */
static inline void mark_dirty(ghostcon_screen_t *s, int16_t y)
{
    s->rows[row_idx(s, y)].dirty = true;
    if (s->dirty.y_min < 0 || y < s->dirty.y_min)
        s->dirty.y_min = y;
    if (s->dirty.y_max < 0 || y > s->dirty.y_max)
        s->dirty.y_max = y;
}

/* Push a line into scrollback history */
static inline void push_history(ghostcon_screen_t *s, ghostcon_row_t *row)
{
    if (s->history_cap == 0)
    {
        ghostcon_row_deinit(row);
        return;
    }

    /* Steal the cell array from the row to avoid reallocation */
    ghostcon_row_t *slot = &s->history[s->history_head];
    /* Free old history cell data if any */
    free(slot->cells);

    *slot = *row; /* struct copy — takes ownership of cells ptr */

    s->history_head = (s->history_head + 1) % s->history_cap;
    if (s->history_count < s->history_cap)
        s->history_count++;
}
