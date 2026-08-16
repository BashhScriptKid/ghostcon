#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cell.h"

typedef struct ghostcon_screen ghostcon_screen_t;

typedef enum {
    GC_SEL_NONE,
    GC_SEL_CHAR,     /* character-wise selection */
    GC_SEL_WORD,     /* word-wise (double-click) */
    GC_SEL_LINE,     /* line-wise (triple-click) */
    GC_SEL_RECT,     /* rectangular (alt+click) */
} ghostcon_sel_kind_t;

typedef struct {
    int16_t x1, y1;  /* start (inclusive) */
    int16_t x2, y2;  /* end   (inclusive) */
    int16_t cols;    /* grid width at selection time */
    ghostcon_sel_kind_t kind;
    bool active;
    bool pending;    /* in-progress drag */
} ghostcon_selection_t;

void ghostcon_selection_clear(ghostcon_selection_t *sel);
void ghostcon_selection_start(ghostcon_selection_t *sel, int16_t x, int16_t y, ghostcon_sel_kind_t kind, uint16_t cols);
void ghostcon_selection_update(ghostcon_selection_t *sel, int16_t x, int16_t y);
void ghostcon_selection_finish(ghostcon_selection_t *sel);
bool ghostcon_selection_contains(const ghostcon_selection_t *sel, int16_t x, int16_t y);

/* Computes the selected column range [*out_xstart, *out_xend] on row
   `y` (both inclusive), given the row's width `cols`. Returns false if
   the selection isn't active or doesn't touch row `y` at all (leaving
   the out-params untouched). Shared by ghostcon_selection_contains(),
   ghostcon_selection_extract_text(), and the renderer's selection
   overlay, so "which columns are selected on row y" has exactly one
   implementation instead of three that could drift apart. */
bool ghostcon_selection_row_range(const ghostcon_selection_t *sel, int16_t y, int16_t cols,
                                   int16_t *out_xstart, int16_t *out_xend);

/* Extracts the currently-selected text as UTF-8 into `out` (NUL-
   terminated, truncated rather than overflowed if it wouldn't fit).
   Returns the number of bytes written (excluding the NUL), or 0 if the
   selection isn't active. A newline separates rows UNLESS the next
   row is a soft-wrap continuation of this one (row_t.wrap_continuation)
   -- matches how the text was actually typed. Trailing blank cells on
   each row are trimmed before its newline. Always reads from the live
   grid (screen->rows), never scrollback history -- selection while
   scrolled back is a known, accepted limitation (see PLAN.md). */
size_t ghostcon_selection_extract_text(ghostcon_screen_t *screen, char *out, size_t out_len);
