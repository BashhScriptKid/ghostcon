#include "ghostcon/term/screen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Convert a 0-based logical row index to the actual index in the ring buffer */
static inline uint16_t
row_idx(ghostcon_screen_t *s, int16_t y) {
    return (uint16_t)((s->scrollback_top + y) % s->rows_visible);
}

/* Ensure y is within scroll region, clamped */
static inline int16_t
clamp_y(ghostcon_screen_t *s, int16_t y) {
    if (y < s->scroll_region.top) y = s->scroll_region.top;
    if (y > s->scroll_region.bottom) y = s->scroll_region.bottom;
    return y;
}

/* Get cell at (x, y) in logical coordinates */
static inline ghostcon_cell_t *
cell_at(ghostcon_screen_t *s, int16_t x, int16_t y) {
    return &s->rows[row_idx(s, y)].cells[x];
}

/* Mark a cell's row as dirty */
static inline void
mark_dirty(ghostcon_screen_t *s, int16_t y) {
    s->rows[row_idx(s, y)].dirty = true;
    if (s->dirty.y_min < 0 || y < s->dirty.y_min)
        s->dirty.y_min = y;
    if (s->dirty.y_max < 0 || y > s->dirty.y_max)
        s->dirty.y_max = y;
}

/* Push a line into scrollback history */
static void
push_history(ghostcon_screen_t *s, ghostcon_row_t *row) {
    if (s->history_cap == 0) {
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

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool
ghostcon_screen_init(ghostcon_screen_t *s,
                     uint16_t cols, uint16_t rows,
                     uint16_t scrollback_cap)
{
    memset(s, 0, sizeof(*s));

    s->cols = cols;
    s->rows_visible = rows;
    s->scrollback_top = 0;

    /* Allocate visible grid (ring buffer) */
    s->rows = (ghostcon_row_t *)calloc(rows, sizeof(ghostcon_row_t));
    if (!s->rows) goto fail;

    for (uint16_t i = 0; i < rows; i++) {
        s->rows[i] = ghostcon_row_init(cols);
        if (!s->rows[i].cells && cols > 0) goto fail;
    }

    /* Allocate history ring buffer */
    if (scrollback_cap > 0) {
        s->history_cap = scrollback_cap;
        s->history = (ghostcon_row_t *)calloc(scrollback_cap, sizeof(ghostcon_row_t));
        if (!s->history) goto fail;

        for (uint16_t i = 0; i < scrollback_cap; i++) {
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
    for (uint16_t i = 8; i < cols; i += 8) {
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

    /* Dirty state: everything, not nothing — a fresh screen still needs
       its first frame painted (uniform background at minimum), same as
       the full-reset convention below (ghostcon_screen_reset). Leaving
       this -1/-1 meant any row never explicitly written to (common —
       most sessions don't fill every row) never got a background quad
       pushed at all on the first frame, showing whatever was already
       in the render target instead of the terminal's actual background
       — found via real hardware testing: "rows that aren't allocated
       by chars have different background than the ones that are
       allocated". */
    s->dirty.y_min = 0;
    s->dirty.y_max = (int16_t)(rows - 1);

    /* Palette */
    ghostcon_palette_init(&s->palette);

    /* Selection */
    ghostcon_selection_clear(&s->selection);

    /* Kitty keyboard protocol */
    ghostcon_kitty_init(&s->kitty);

    /* Mode bitfield */
    ghostcon_modes_clear(&s->modes);
    if (s->auto_wrap) ghostcon_modes_set(&s->modes, GC_MODE_AUTO_WRAP);

    return true;

fail:
    ghostcon_screen_deinit(s);
    return false;
}

void
ghostcon_screen_deinit(ghostcon_screen_t *s) {
    if (s->rows) {
        for (uint16_t i = 0; i < s->rows_visible; i++)
            ghostcon_row_deinit(&s->rows[i]);
        free(s->rows);
    }
    if (s->history) {
        for (uint16_t i = 0; i < s->history_cap; i++)
            ghostcon_row_deinit(&s->history[i]);
        free(s->history);
    }
    if (s->alt_rows) {
        for (uint16_t i = 0; i < s->alt_rows_visible; i++)
            ghostcon_row_deinit(&s->alt_rows[i]);
        free(s->alt_rows);
    }
    ghostcon_style_set_destroy(s->styles);
    ghostcon_hyperlink_set_destroy(s->hyperlinks);
    free(s->tabstops.stops);
    memset(s, 0, sizeof(*s));
}

bool
ghostcon_screen_resize(ghostcon_screen_t *s,
                       uint16_t new_cols, uint16_t new_rows)
{
    /* TODO: proper reflow — for now, simple truncate/extend */
    if (new_cols != s->cols) {
        for (uint16_t i = 0; i < s->rows_visible; i++)
            ghostcon_row_resize(&s->rows[i], new_cols);
        if (s->history) {
            for (uint16_t i = 0; i < s->history_cap; i++)
                ghostcon_row_resize(&s->history[i], new_cols);
        }
    }

    if (new_rows != s->rows_visible) {
        /* Deinit rows being removed BEFORE realloc */
        if (new_rows < s->rows_visible) {
            for (uint16_t i = new_rows; i < s->rows_visible; i++)
                ghostcon_row_deinit(&s->rows[i]);
        }

        ghostcon_row_t *new_grid = (ghostcon_row_t *)realloc(
            s->rows, new_rows * sizeof(ghostcon_row_t));
        if (!new_grid) return false;

        if (new_rows > s->rows_visible) {
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
    if (new_cols > s->tabstops.cols) {
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

/* ------------------------------------------------------------------ */
/* Cursor movement                                                     */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_cursor_up(ghostcon_screen_t *s, uint16_t n) {
    s->cursor.y -= (int16_t)n;
    if (s->cursor.y < s->scroll_region.top)
        s->cursor.y = s->scroll_region.top;
}

void
ghostcon_screen_cursor_down(ghostcon_screen_t *s, uint16_t n) {
    s->cursor.y += (int16_t)n;
    if (s->cursor.y > s->scroll_region.bottom)
        s->cursor.y = s->scroll_region.bottom;
}

void
ghostcon_screen_cursor_left(ghostcon_screen_t *s, uint16_t n) {
    s->cursor.x -= (int16_t)n;
    if (s->cursor.x < 0)
        s->cursor.x = 0;
}

void
ghostcon_screen_cursor_right(ghostcon_screen_t *s, uint16_t n) {
    s->cursor.x += (int16_t)n;
    if (s->cursor.x >= (int16_t)s->cols)
        s->cursor.x = (int16_t)(s->cols - 1);
}

void
ghostcon_screen_cursor_set(ghostcon_screen_t *s, int16_t x, int16_t y) {
    if (s->origin_mode) {
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

void
ghostcon_screen_cursor_horizontal_abs(ghostcon_screen_t *s, int16_t x) {
    if (s->origin_mode)
        x += s->margin_region.left;
    if (x < 0) x = 0;
    if (x >= (int16_t)s->cols) x = (int16_t)(s->cols - 1);
    s->cursor.x = x;
}

void
ghostcon_screen_cursor_vertical_abs(ghostcon_screen_t *s, int16_t y) {
    /* VPA (CSI Ps d): row absolute, column unchanged. With origin mode
       the row is relative to the scroll region and clamped to it
       (mirrors Ghostty's setCursorPos). */
    int16_t y_max = (int16_t)(s->rows_visible - 1);
    if (s->origin_mode) {
        y += s->scroll_region.top;
        y_max = s->scroll_region.bottom;
    }
    if (y < 0) y = 0;
    if (y > y_max) y = y_max;
    s->cursor.y = y;
    s->cursor.pending_wrap = false;
}

void
ghostcon_screen_cursor_next_line(ghostcon_screen_t *s) {
    s->cursor.x = s->margin_region.left;
    s->cursor.y++;
    if (s->cursor.y > s->scroll_region.bottom)
        ghostcon_screen_cursor_scroll_up(s);
    s->cursor.pending_wrap = false;
}

void
ghostcon_screen_cursor_prev_line(ghostcon_screen_t *s) {
    s->cursor.x = s->margin_region.left;
    s->cursor.y--;
    if (s->cursor.y < s->scroll_region.top)
        s->cursor.y = s->scroll_region.top;
}

void
ghostcon_screen_cursor_scroll_up(ghostcon_screen_t *s) {
    ghostcon_screen_scroll_up(s, 1);
}

void
ghostcon_screen_cursor_scroll_down(ghostcon_screen_t *s) {
    ghostcon_screen_scroll_down(s, 1);
}

void
ghostcon_screen_cursor_save(ghostcon_screen_t *s) {
    s->saved_cursor.x = s->cursor.x;
    s->saved_cursor.y = s->cursor.y;
    s->saved_cursor.cursor_style = s->cursor.cursor_style;
    s->saved_cursor.pending_wrap = s->cursor.pending_wrap;
    s->saved_cursor.style_id = s->cursor.style_id;
    s->cursor_saved = true;
}

void
ghostcon_screen_cursor_restore(ghostcon_screen_t *s) {
    if (!s->cursor_saved) return;
    s->cursor.x = s->saved_cursor.x;
    s->cursor.y = s->saved_cursor.y;
    s->cursor.cursor_style = s->saved_cursor.cursor_style;
    s->cursor.pending_wrap = s->saved_cursor.pending_wrap;
    s->cursor.style_id = s->saved_cursor.style_id;
}

/* ------------------------------------------------------------------ */
/* Text insertion                                                      */
/* ------------------------------------------------------------------ */

/* Print a cell at the cursor position with the given wide state
   (mirrors Ghostty's Terminal.printCell). Clears stale wide spacers
   left behind when a cell's wide state changes. */
static void
print_cell(ghostcon_screen_t *s, uint32_t cp, ghostcon_cell_wide_t wide) {
    ghostcon_cell_t *cell = cell_at(s, s->cursor.x, s->cursor.y);

    if (ghostcon_cell_get_wide(*cell) != wide) {
        switch (ghostcon_cell_get_wide(*cell)) {
        case GHOSTCON_CELL_WIDE_NARROW:
            break;
        case GHOSTCON_CELL_WIDE_WIDE:
            /* Clear the spacer tail to the right */
            if (s->cursor.x < (int16_t)s->cols - 1)
                cell_at(s, s->cursor.x + 1, s->cursor.y)->raw = 0;
            /* Clear a stale spacer_head at the end of the previous row */
            if (s->cursor.y > 0 && s->cursor.x <= 1) {
                ghostcon_cell_t *head = cell_at(s, (int16_t)s->cols - 1, s->cursor.y - 1);
                if (ghostcon_cell_get_wide(*head) == GHOSTCON_CELL_WIDE_SPACER_HEAD)
                    ghostcon_cell_set_wide(head, GHOSTCON_CELL_WIDE_NARROW);
            }
            break;
        case GHOSTCON_CELL_WIDE_SPACER_TAIL:
            /* Clear the wide cell to the left */
            if (s->cursor.x > 0)
                cell_at(s, s->cursor.x - 1, s->cursor.y)->raw = 0;
            /* Clear a stale spacer_head at the end of the previous row */
            if (s->cursor.y > 0 && s->cursor.x <= 1) {
                ghostcon_cell_t *head = cell_at(s, (int16_t)s->cols - 1, s->cursor.y - 1);
                if (ghostcon_cell_get_wide(*head) == GHOSTCON_CELL_WIDE_SPACER_HEAD)
                    ghostcon_cell_set_wide(head, GHOSTCON_CELL_WIDE_NARROW);
            }
            break;
        case GHOSTCON_CELL_WIDE_SPACER_HEAD:
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

/* Soft wrap: move the cursor to the start of the next line, marking the
   row as wrapped (mirrors Ghostty's printWrap). */
static void
soft_wrap(ghostcon_screen_t *s) {
    bool mark_wrap = (s->cursor.x == (int16_t)s->cols - 1);
    if (mark_wrap)
        s->rows[row_idx(s, s->cursor.y)].wrap = true;

    if (s->cursor.y == s->scroll_region.bottom) {
        ghostcon_screen_scroll_up(s, 1);
    } else {
        s->cursor.y++;
    }
    s->cursor.x = s->margin_region.left;
    s->cursor.pending_wrap = false;

    if (mark_wrap)
        s->rows[row_idx(s, s->cursor.y)].wrap_continuation = true;
}

void
ghostcon_screen_put_char(ghostcon_screen_t *s, uint32_t codepoint) {
    /* Our right margin depends where our cursor is now (Ghostty's
       right_limit): if the cursor was placed past the margin by an
       absolute-position command, wrap at the real screen edge. */
    int16_t right_limit = s->cursor.x > s->margin_region.right
        ? (int16_t)s->cols
        : (int16_t)(s->margin_region.right + 1);

    uint8_t width = ghostcon_unicode_width(codepoint);

    /* Zero-width characters are attached as grapheme data to the
       previous cell (mirrors Ghostty's print). */
    if (width == 0) {
        int left = (s->auto_wrap && s->cursor.pending_wrap) ? 0 : 1;
        if (s->cursor.x == 0 && left == 1) return;

        ghostcon_cell_t *prev = cell_at(s, s->cursor.x - left, s->cursor.y);
        if (ghostcon_cell_get_wide(*prev) == GHOSTCON_CELL_WIDE_SPACER_TAIL)
            prev = cell_at(s, s->cursor.x - left - 1, s->cursor.y);
        if (!ghostcon_cell_has_text(*prev)) return;

        /* No grapheme storage yet (Ghostty stores combining sequences
           on the page) — tag the cell so the renderer can resolve the
           full sequence once storage lands. */
        ghostcon_cell_set_tag(prev, GHOSTCON_CELL_CODEPOINT_GRAPHEME);
        s->rows[row_idx(s, s->cursor.y)].grapheme = true;
        mark_dirty(s, s->cursor.y);
        return;
    }

    /* Handle pending soft-wrap */
    if (s->cursor.pending_wrap && s->auto_wrap)
        soft_wrap(s);

    /* Insert mode shifts cells right by the character width */
    if (s->insert_mode && s->cursor.x + (int16_t)width < (int16_t)s->cols) {
        ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
        int16_t right = s->margin_region.right;
        int16_t shift = width;
        if (s->cursor.x + shift > right + 1)
            shift = right - s->cursor.x + 1;
        if (shift > 0) {
            memmove(&r->cells[s->cursor.x + shift], &r->cells[s->cursor.x],
                    (size_t)(right - s->cursor.x + 1 - shift) * sizeof(ghostcon_cell_t));
            for (int16_t i = 0; i < shift; i++)
                r->cells[s->cursor.x + i] = GHOSTCON_CELL_EMPTY;
            mark_dirty(s, s->cursor.y);
        }
    }

    switch (width) {
    case 1:
        print_cell(s, codepoint, GHOSTCON_CELL_WIDE_NARROW);
        s->last_codepoint = codepoint;
        break;
    /* Wide characters require a spacer: the first cell is flagged wide
       and holds the character, the second is a spacer tail. */
    case 2:
        if ((right_limit - s->margin_region.left) > 1) {
            if (s->cursor.x == right_limit - 1) {
                /* Not enough room — insert a spacer and wrap first. */
                if (!s->auto_wrap) return;
                if (right_limit == (int16_t)s->cols)
                    print_cell(s, 0, GHOSTCON_CELL_WIDE_SPACER_HEAD);
                else
                    print_cell(s, 0, GHOSTCON_CELL_WIDE_NARROW);
                soft_wrap(s);
            }
            print_cell(s, codepoint, GHOSTCON_CELL_WIDE_WIDE);
            s->cursor.x++;
            if (s->cursor.x >= (int16_t)s->cols)
                s->cursor.x = (int16_t)(s->cols - 1);
            print_cell(s, 0, GHOSTCON_CELL_WIDE_SPACER_TAIL);
        } else {
            print_cell(s, 0, GHOSTCON_CELL_WIDE_NARROW);
        }
        s->last_codepoint = codepoint;
        break;

    default:
        return;
    }

    /* If we're at the column limit, set pending wrap — the wrap itself
       is deferred until the next character (matches Ghostty). */
    if (s->cursor.x == right_limit - 1) {
        s->cursor.pending_wrap = true;
        return;
    }

    s->cursor.x++;
}

void
ghostcon_screen_put_text(ghostcon_screen_t *s,
                         const uint32_t *codepoints, size_t len)
{
    for (size_t i = 0; i < len; i++)
        ghostcon_screen_put_char(s, codepoints[i]);
}

void
ghostcon_screen_carriage_return(ghostcon_screen_t *s) {
    s->cursor.x = s->margin_region.left;
    s->cursor.pending_wrap = false;
}

void
ghostcon_screen_linefeed(ghostcon_screen_t *s) {
    s->cursor.pending_wrap = false;
    if (s->cursor.y == s->scroll_region.bottom) {
        ghostcon_screen_scroll_up(s, 1);
    } else {
        s->cursor.y++;
    }
}

void
ghostcon_screen_reverse_index(ghostcon_screen_t *s) {
    if (s->cursor.y == s->scroll_region.top) {
        ghostcon_screen_scroll_down(s, 1);
    } else {
        s->cursor.y--;
    }
}

void
ghostcon_screen_tab(ghostcon_screen_t *s) {
    /* Move to next tab stop, or to margin right if none */
    int16_t start = s->cursor.x + 1;
    for (int16_t i = start; i <= s->margin_region.right; i++) {
        if (i < (int16_t)s->tabstops.cols && s->tabstops.stops[i]) {
            s->cursor.x = i;
            return;
        }
    }
    /* No tab stop found — go to margin right */
    s->cursor.x = s->margin_region.right;
}

void
ghostcon_screen_tab_back(ghostcon_screen_t *s) {
    int16_t start = s->cursor.x - 1;
    for (int16_t i = start; i >= 0; i--) {
        if (i < (int16_t)s->tabstops.cols && s->tabstops.stops[i]) {
            s->cursor.x = i;
            return;
        }
    }
    s->cursor.x = 0;
}

/* ------------------------------------------------------------------ */
/* Erase operations                                                    */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_erase_display(ghostcon_screen_t *s, int mode) {
    int16_t top, bottom;

    switch (mode) {
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
        break;
    case GC_ERASE_DISPLAY_SCROLLBACK:
        /* Clear scrollback history */
        if (s->history) {
            for (uint16_t i = 0; i < s->history_count; i++)
                ghostcon_row_clear(&s->history[i]);
            s->history_count = 0;
        }
        return;
    case GC_ERASE_DISPLAY_SCROLL_COMPLETE:
        /* Scroll entire display into scrollback */
        for (int16_t i = 0; i < (int16_t)s->rows_visible; i++) {
            ghostcon_row_t *r = &s->rows[row_idx(s, i)];
            if (ghostcon_row_init(s->cols).cells) {
                push_history(s, r);
            }
            ghostcon_row_clear(r);
            mark_dirty(s, i);
        }
        return;
    default:
        return;
    }

    for (int16_t y = top; y <= bottom; y++) {
        ghostcon_row_t *r = &s->rows[row_idx(s, y)];
        if (y == s->cursor.y && mode != GC_ERASE_DISPLAY_ALL) {
            /* Erase from cursor x to end of line */
            uint16_t left = (mode == GC_ERASE_DISPLAY_ABOVE) ? 0 : s->cursor.x;
            uint16_t end  = (mode == GC_ERASE_DISPLAY_ABOVE) ? (uint16_t)(s->cursor.x + 1) : s->cols;
            ghostcon_row_clear_range(r, left, end);
            /* Ghostty's ED.below calls eraseLine(.right) which resets the
               cursor row's soft-wrap; ED.above calls eraseLine(.left)
               which preserves it. */
            if (mode == GC_ERASE_DISPLAY_BELOW) {
                r->wrap = false;
                r->wrap_continuation = false;
            }
        } else {
            ghostcon_row_clear(r);
        }
        mark_dirty(s, y);
    }
}

void
ghostcon_screen_erase_display_protected(ghostcon_screen_t *s, int mode) {
    int16_t top, bottom;

    switch (mode) {
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
    for (int16_t y = top; y <= bottom; y++) {
        ghostcon_row_t *r = &s->rows[row_idx(s, y)];
        if (y == s->cursor.y && mode != GC_ERASE_DISPLAY_ALL) {
            uint16_t left = (mode == GC_ERASE_DISPLAY_ABOVE) ? 0 : s->cursor.x;
            uint16_t end  = (mode == GC_ERASE_DISPLAY_ABOVE) ? (uint16_t)(s->cursor.x + 1) : s->cols;
            ghostcon_row_clear_range_unprotected(r, left, end);
        } else {
            ghostcon_row_clear_range_unprotected(r, 0, s->cols);
        }
        mark_dirty(s, y);
    }
}

void
ghostcon_screen_erase_line(ghostcon_screen_t *s, int mode) {
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t left, end;

    switch (mode) {
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

    ghostcon_row_clear_range(r, left, end);
    /* Ghostty's eraseLine(.right) resets the row's soft-wrap state
       (cursorResetWrap); .left and .complete preserve it. */
    if (mode == GC_ERASE_LINE_RIGHT) {
        r->wrap = false;
        r->wrap_continuation = false;
    }
    mark_dirty(s, s->cursor.y);
}

void
ghostcon_screen_erase_line_protected(ghostcon_screen_t *s, int mode) {
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t left, end;

    switch (mode) {
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
    ghostcon_row_clear_range_unprotected(r, left, end);
    mark_dirty(s, s->cursor.y);
}

void
ghostcon_screen_save_mode(ghostcon_screen_t *s, int mode) {
    switch (mode) {
    case 1:  s->saved_modes.application_cursor  = s->application_cursor; break; /* DECCKM */
    case 4:  s->saved_modes.insert_mode         = s->insert_mode; break;        /* IRM */
    case 5:  s->saved_modes.reverse_video       = s->reverse_video; break;      /* DECSCNM */
    case 6:  s->saved_modes.origin_mode         = s->origin_mode; break;        /* DECOM */
    case 7:  s->saved_modes.auto_wrap           = s->auto_wrap; break;          /* DECAWM */
    case 69: s->saved_modes.left_right_margin   = s->left_right_margin; break;  /* DECLRMM */
    default: break;
    }
}

void
ghostcon_screen_restore_mode(ghostcon_screen_t *s, int mode) {
    switch (mode) {
    case 1:  s->application_cursor = s->saved_modes.application_cursor; break;
    case 4:  s->insert_mode        = s->saved_modes.insert_mode; break;
    case 5:  s->reverse_video      = s->saved_modes.reverse_video; break;
    case 6:  s->origin_mode        = s->saved_modes.origin_mode; break;
    case 7:  s->auto_wrap          = s->saved_modes.auto_wrap; break;
    case 69: s->left_right_margin  = s->saved_modes.left_right_margin; break;
    default: break;
    }
}

void
ghostcon_screen_erase_chars(ghostcon_screen_t *s, uint16_t n) {
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    uint16_t end = s->cursor.x + n;
    if (end > s->cols) end = s->cols;
    ghostcon_row_clear_range(r, s->cursor.x, end);
    mark_dirty(s, s->cursor.y);
}

void
ghostcon_screen_insert_chars(ghostcon_screen_t *s, uint16_t n) {
    if (n == 0) return;
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    int16_t right = s->margin_region.right;

    /* Clamp n to available space to the right of the cursor */
    if (s->cursor.x + (int16_t)n > right + 1)
        n = (uint16_t)(right - s->cursor.x + 1);

    /* Shift cells right from the cursor */
    memmove(&r->cells[s->cursor.x + n], &r->cells[s->cursor.x],
            (right - s->cursor.x + 1 - (int16_t)n) * sizeof(ghostcon_cell_t));

    /* Clear the inserted cells */
    for (int16_t i = s->cursor.x; i < s->cursor.x + (int16_t)n; i++)
        r->cells[i] = GHOSTCON_CELL_EMPTY;

    mark_dirty(s, s->cursor.y);
}

void
ghostcon_screen_delete_chars(ghostcon_screen_t *s, uint16_t n) {
    ghostcon_row_t *r = &s->rows[row_idx(s, s->cursor.y)];
    int16_t right = s->margin_region.right;

    /* Shift cells left from cursor+n */
    uint16_t src = s->cursor.x + n;
    if (src > (uint16_t)right) src = right;
    uint16_t count = right - src + 1;

    if (count > 0 && src <= (uint16_t)right) {
        memmove(&r->cells[s->cursor.x], &r->cells[src], count * sizeof(ghostcon_cell_t));
    }
    for (int16_t i = right - n + 1; i <= right; i++)
        if (i >= 0) r->cells[i] = GHOSTCON_CELL_EMPTY;

    mark_dirty(s, s->cursor.y);
}

/* ------------------------------------------------------------------ */
/* Insert/Delete lines                                                 */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_insert_lines(ghostcon_screen_t *s, uint16_t n) {
    if (n == 0) return;
    int16_t top = s->cursor.y;
    int16_t bottom = s->scroll_region.bottom;

    /* Scroll lines [top, bottom-n] down by n */
    for (int16_t y = bottom; y >= top + (int16_t)n; y--) {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y - n)];
        /* Copy cells */
        memcpy(dst->cells, src->cells, s->cols * sizeof(ghostcon_cell_t));
        dst->wrap = src->wrap;
        dst->wrap_continuation = src->wrap_continuation;
        dst->grapheme = src->grapheme;
        dst->styled = src->styled;
        dst->hyperlink = src->hyperlink;
        dst->dirty = true;
        mark_dirty(s, y);
    }

    /* Clear the top n lines */
    for (int16_t y = top; y < top + (int16_t)n && y <= bottom; y++) {
        ghostcon_row_clear(&s->rows[row_idx(s, y)]);
        mark_dirty(s, y);
    }
}

void
ghostcon_screen_delete_lines(ghostcon_screen_t *s, uint16_t n) {
    if (n == 0) return;
    int16_t top = s->cursor.y;
    int16_t bottom = s->scroll_region.bottom;

    /* Scroll lines [top+n, bottom] up by n */
    for (int16_t y = top; y <= bottom - (int16_t)n; y++) {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y + n)];
        memcpy(dst->cells, src->cells, s->cols * sizeof(ghostcon_cell_t));
        dst->wrap = src->wrap;
        dst->wrap_continuation = src->wrap_continuation;
        dst->grapheme = src->grapheme;
        dst->styled = src->styled;
        dst->hyperlink = src->hyperlink;
        dst->dirty = true;
        mark_dirty(s, y);
    }

    /* Clear the bottom n lines */
    for (int16_t y = bottom - (int16_t)n + 1; y <= bottom; y++) {
        ghostcon_row_clear(&s->rows[row_idx(s, y)]);
        mark_dirty(s, y);
    }
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_scroll_up(ghostcon_screen_t *s, uint16_t n) {
    int16_t top = s->scroll_region.top;
    int16_t bottom = s->scroll_region.bottom;

    if (top > bottom)
        return;

    int16_t region_height = bottom - top + 1;
    if (n > (uint16_t)region_height)
        n = region_height;

    for (uint16_t i = 0; i < n; i++) {
        /* Push the top line of the scroll region into history */
        ghostcon_row_t *top_row = &s->rows[row_idx(s, top + i)];

        /* Create a copy to push into history */
        ghostcon_row_t hist_copy = ghostcon_row_init(s->cols);
        if (hist_copy.cells) {
            memcpy(hist_copy.cells, top_row->cells, s->cols * sizeof(ghostcon_cell_t));
            hist_copy.wrap = top_row->wrap;
            hist_copy.wrap_continuation = top_row->wrap_continuation;
            push_history(s, &hist_copy);
        } else {
            /* If allocation fails, just lose the history */
        }
    }

    /* Shift rows down (bottom row gets cleared) */
    for (int16_t y = top; y <= bottom - (int16_t)n; y++) {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y + n)];
        memcpy(dst->cells, src->cells, s->cols * sizeof(ghostcon_cell_t));
        dst->wrap = src->wrap;
        dst->wrap_continuation = src->wrap_continuation;
        dst->grapheme = src->grapheme;
        dst->styled = src->styled;
        dst->hyperlink = src->hyperlink;
        dst->dirty = true;
        mark_dirty(s, y);
    }

    /* Clear the bottom n lines of the scroll region */
    for (int16_t y = bottom - (int16_t)n + 1; y <= bottom; y++) {
        ghostcon_row_clear(&s->rows[row_idx(s, y)]);
        mark_dirty(s, y);
    }
}

void
ghostcon_screen_scroll_down(ghostcon_screen_t *s, uint16_t n) {
    int16_t top = s->scroll_region.top;
    int16_t bottom = s->scroll_region.bottom;

    if (top > bottom)
        return;

    int16_t region_height = bottom - top + 1;
    if (n > (uint16_t)region_height)
        n = region_height;

    /* Shift rows up (top line gets cleared) */
    for (int16_t y = bottom; y >= top + (int16_t)n; y--) {
        ghostcon_row_t *dst = &s->rows[row_idx(s, y)];
        ghostcon_row_t *src = &s->rows[row_idx(s, y - n)];
        memcpy(dst->cells, src->cells, s->cols * sizeof(ghostcon_cell_t));
        dst->wrap = src->wrap;
        dst->wrap_continuation = src->wrap_continuation;
        dst->grapheme = src->grapheme;
        dst->styled = src->styled;
        dst->hyperlink = src->hyperlink;
        dst->dirty = true;
        mark_dirty(s, y);
    }

    /* Clear the top n lines of the scroll region */
    for (int16_t y = top; y < top + (int16_t)n && y <= bottom; y++) {
        ghostcon_row_clear(&s->rows[row_idx(s, y)]);
        mark_dirty(s, y);
    }
}

void
ghostcon_screen_set_scroll_region(ghostcon_screen_t *s,
                                  int16_t top, int16_t bottom)
{
    if (top < 0 || bottom < 0) {
        /* Reset to full screen */
        s->scroll_region.top = 0;
        s->scroll_region.bottom = (int16_t)(s->rows_visible - 1);
    } else {
        s->scroll_region.top = top;
        s->scroll_region.bottom = bottom;
    }

    /* Cursor goes to home position: region home if origin mode,
       otherwise screen home (matches Ghostty setCursorPos(1,1)). */
    if (s->origin_mode) {
        s->cursor.x = s->margin_region.left;
        s->cursor.y = s->scroll_region.top;
    } else {
        s->cursor.x = 0;
        s->cursor.y = 0;
    }
    s->cursor.pending_wrap = false;
}

void
ghostcon_screen_set_margin_region(ghostcon_screen_t *s,
                                  int16_t left, int16_t right)
{
    if (left < 0 || right < 0) {
        s->margin_region.left = 0;
        s->margin_region.right = (int16_t)(s->cols - 1);
    } else {
        s->margin_region.left = left;
        s->margin_region.right = right;
    }
}

/* ------------------------------------------------------------------ */
/* Alternate screen                                                    */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_alt_screen_enter(ghostcon_screen_t *s) {
    if (s->alt_screen_active)
        return;

    /* Save main screen grid */
    s->alt_rows_visible = s->rows_visible;
    s->alt_rows = (ghostcon_row_t *)calloc(s->rows_visible, sizeof(ghostcon_row_t));
    if (!s->alt_rows) return;

    for (uint16_t i = 0; i < s->rows_visible; i++) {
        s->alt_rows[i] = ghostcon_row_init(s->cols);
        if (s->alt_rows[i].cells && s->rows[i].cells) {
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
    for (uint16_t i = 0; i < s->rows_visible; i++) {
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

void
ghostcon_screen_alt_screen_exit(ghostcon_screen_t *s) {
    if (!s->alt_screen_active)
        return;

    /* Restore main screen grid */
    for (uint16_t i = 0; i < s->rows_visible && i < s->alt_rows_visible; i++) {
        if (s->rows[i].cells && s->alt_rows[i].cells) {
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

/* ------------------------------------------------------------------ */
/* Tab stops                                                           */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_tab_clear(ghostcon_screen_t *s, int mode) {
    /* mode 0 = clear at cursor, mode 3 = clear all */
    if (mode == 3) {
        memset(s->tabstops.stops, 0, s->tabstops.cols * sizeof(bool));
    } else {
        if (s->cursor.x < (int16_t)s->tabstops.cols)
            s->tabstops.stops[s->cursor.x] = false;
    }
}

void
ghostcon_screen_tab_set(ghostcon_screen_t *s) {
    if (s->cursor.x < (int16_t)s->tabstops.cols)
        s->tabstops.stops[s->cursor.x] = true;
}

/* ------------------------------------------------------------------ */
/* Attribute/style                                                     */
/* ------------------------------------------------------------------ */

ghostcon_style_id_t
ghostcon_screen_set_style(ghostcon_screen_t *s,
                          ghostcon_style_id_t style_id)
{
    s->cursor.style_id = style_id;
    return style_id;
}

/* Apply an SGR attribute — this is a thin wrapper that will be expanded
   when integrated with libghostty-vt's SGR parser. For now, a no-op. */
ghostcon_style_t
ghostcon_screen_apply_sgr_attribute(const ghostcon_style_t *base,
                                   int sgr_tag,
                                   const void *sgr_value)
{
    (void)sgr_tag;
    (void)sgr_value;
    return *base;
}

/* ------------------------------------------------------------------ */
/* Damage tracking                                                     */
/* ------------------------------------------------------------------ */

void
ghostcon_screen_mark_dirty(ghostcon_screen_t *s, uint16_t y) {
    s->rows[row_idx(s, (int16_t)y)].dirty = true;
    if (s->dirty.y_min < 0 || (int16_t)y < s->dirty.y_min)
        s->dirty.y_min = (int16_t)y;
    if (s->dirty.y_max < 0 || (int16_t)y > s->dirty.y_max)
        s->dirty.y_max = (int16_t)y;
}

void
ghostcon_screen_clear_dirty(ghostcon_screen_t *s) {
    s->dirty.y_min = -1;
    s->dirty.y_max = -1;
}

ghostcon_dirty_region_t
ghostcon_screen_get_dirty(const ghostcon_screen_t *s) {
    return s->dirty;
}

/* ------------------------------------------------------------------ */
/* Direct cell access                                                  */
/* ------------------------------------------------------------------ */

ghostcon_cell_t *
ghostcon_screen_cell(ghostcon_screen_t *s, uint16_t x, uint16_t y) {
    if (y >= s->rows_visible || x >= s->cols)
        return NULL;
    return &s->rows[row_idx(s, (int16_t)y)].cells[x];
}

ghostcon_row_t *
ghostcon_screen_row(ghostcon_screen_t *s, uint16_t y) {
    if (y >= s->rows_visible)
        return NULL;
    return &s->rows[row_idx(s, (int16_t)y)];
}
