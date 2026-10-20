#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cell.h"

/* Row
 *
 * a single row of cells in the terminal grid
 *
 * Simpler than Ghostty's packed u64 Row: instead of storing an offset into mmap'd
 * page memory, we own the cell array directly. The packing optimization can be
 * adopted later if needed.
 */

typedef struct
{
    ghostcon_cell_t *cells;   /* array of cells, length = cols         */
    uint16_t         cols;     /* number of columns (allocated length)  */
    bool             dirty;    /* needs redraw                          */
    bool             wrap;     /* soft-wrapped to next row              */
    bool             wrap_continuation;  /* continuation from prev row  */
    bool             grapheme; /* has multi-codepoint grapheme cells    */
    bool             styled;   /* has non-default-style cells           */
    bool             hyperlink;/* has hyperlink cells                   */
} ghostcon_row_t;

/* Initialize a row with `cols` cells (zero-initialized) */
extern ghostcon_row_t ghostcon_row_init(uint16_t cols);

/* Free a row's cell storage (does NOT free the row struct itself) */
extern void ghostcon_row_deinit(ghostcon_row_t *row);

/* Resize row to new column count, preserving contents up to min(cols,
 * new_cols) Returns true on success, false on allocation failure (row is
 * unchanged)
 */
extern bool ghostcon_row_resize(ghostcon_row_t *row, uint16_t new_cols);

/* Clear cells in range [left, end) — resets to empty cells */
extern void ghostcon_row_clear_range(ghostcon_row_t *row, uint16_t left, uint16_t end);
/* Clear only unprotected cells in range [left, end) (DECSED/DECSEL) */
extern void ghostcon_row_clear_range_unprotected(ghostcon_row_t *row, uint16_t left, uint16_t end);
/* Clear all cells in the row */
extern void ghostcon_row_clear(ghostcon_row_t *row);

/* Fill cells in range [left, end) with `fill` (the ECMA-48 "erase color" cell
 *
 * normally an empty cell carrying only the currently selected background, built
 * by the caller).
 *
 * Same bounds/dirty/metadata semantics as ghostcon_row_clear_range.
 */
extern void ghostcon_row_fill_range(ghostcon_row_t *row, uint16_t left, uint16_t end, ghostcon_cell_t fill);
/* Fill only unprotected cells in range [left, end) with `fill` (DECSED/DECSEL) */
extern void ghostcon_row_fill_range_unprotected(ghostcon_row_t *row, uint16_t left, uint16_t end, ghostcon_cell_t fill);
/* Fill all cells in the row with `fill` */
extern void ghostcon_row_fill(ghostcon_row_t *row, ghostcon_cell_t fill);

/* Copy a row's contents and metadata into another row.
 *
 * Copies min(dst->cols, src->cols) cells, and the wrap / grapheme / styled /
 * hyperlink metadata with them. The destination is marked dirty.
 *
 * This is the row shift that the four grid-scrolling operations (insert
 * lines, delete lines, scroll up, scroll down) all perform; they were four
 * byte-identical copies of the same seven assignments.
 */
extern void ghostcon_row_copy(ghostcon_row_t *dst, const ghostcon_row_t *src);
