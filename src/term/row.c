#include "ghostcon/term/row.h"
#include "ghostcon/term/style.h"
#include <stdlib.h>
#include <string.h>

ghostcon_row_t
ghostcon_row_init(uint16_t cols)
{
    ghostcon_row_t row = {
        .cells = NULL,
        .cols = cols,
        .dirty = false,
        .wrap = false,
        .wrap_continuation = false,
        .grapheme = false,
        .styled = false,
        .hyperlink = false,
    };

    if (cols > 0)
    {
        row.cells = (ghostcon_cell_t *)calloc(cols, sizeof(ghostcon_cell_t));
    }
    return row;
}

void ghostcon_row_deinit(ghostcon_row_t *row)
{
    free(row->cells);
    row->cells = NULL;
    row->cols = 0;
}

bool ghostcon_row_resize(ghostcon_row_t *row, uint16_t new_cols)
{
    if (new_cols == row->cols)
        return true;

    ghostcon_cell_t *new_cells = (ghostcon_cell_t *)calloc(new_cols, sizeof(ghostcon_cell_t));
    if (!new_cells)
        return false;

    uint16_t copy = new_cols < row->cols ? new_cols : row->cols;
    if (copy > 0 && row->cells)
    {
        memcpy(new_cells, row->cells, copy * sizeof(ghostcon_cell_t));
    }

    free(row->cells);
    row->cells = new_cells;
    row->cols = new_cols;
    row->dirty = true;
    return true;
}

void ghostcon_row_fill_range(ghostcon_row_t *row, uint16_t left, uint16_t end, ghostcon_cell_t fill)
{
    if (left >= row->cols || end > row->cols || left >= end)
        return;

    for (uint16_t i = left; i < end; i++)
        row->cells[i] = fill;
    row->dirty = true;

    /* If we cleared the entire row, reset the metadata flags */
    if (left == 0 && end == row->cols)
    {
        row->grapheme = false;
        row->styled = ghostcon_cell_get_style(fill) != GC_STYLE_DEFAULT_ID ||
                      ghostcon_cell_get_tag(fill) != GC_CELL_CODEPOINT;
        row->hyperlink = false;
    }
}

void ghostcon_row_clear_range(ghostcon_row_t *row, uint16_t left, uint16_t end)
{
    ghostcon_row_fill_range(row, left, end, (ghostcon_cell_t){0});
}

void ghostcon_row_fill(ghostcon_row_t *row, ghostcon_cell_t fill)
{
    ghostcon_row_fill_range(row, 0, row->cols, fill);

    /* A fully-cleared row is a fresh blank row: reset wrap state too
     * (matches Ghostty's clearRows/scroll semantics where ED and scroll
     * erase produce unwrapped rows).
     *
     * EL/ECH go through row_clear_range and intentionally preserve wrap.
     */
    row->wrap = false;
    row->wrap_continuation = false;
}

void ghostcon_row_clear(ghostcon_row_t *row)
{
    ghostcon_row_fill(row, (ghostcon_cell_t){0});
}

void ghostcon_row_fill_range_unprotected(ghostcon_row_t *row, uint16_t left, uint16_t end, ghostcon_cell_t fill)
{
    if (left >= row->cols || end > row->cols || left >= end)
        return;

    for (uint16_t i = left; i < end; i++)
    {
        if (!ghostcon_cell_get_protected(row->cells[i]))
            row->cells[i] = fill;
    }
    row->dirty = true;
}

void ghostcon_row_clear_range_unprotected(ghostcon_row_t *row, uint16_t left, uint16_t end)
{
    ghostcon_row_fill_range_unprotected(row, left, end, (ghostcon_cell_t){0});
}

void ghostcon_row_copy(ghostcon_row_t *dst, const ghostcon_row_t *src)
{
    uint16_t cols = dst->cols < src->cols ? dst->cols : src->cols;
    if (cols > 0 && dst->cells && src->cells)
        memcpy(dst->cells, src->cells, cols * sizeof(ghostcon_cell_t));
    dst->wrap = src->wrap;
    dst->wrap_continuation = src->wrap_continuation;
    dst->grapheme = src->grapheme;
    dst->styled = src->styled;
    dst->hyperlink = src->hyperlink;
    dst->dirty = true;
}
