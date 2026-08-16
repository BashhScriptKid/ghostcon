#include "ghostcon/term/selection.h"

#include "ghostcon/term/screen.h"

void
ghostcon_selection_clear(ghostcon_selection_t *sel) {
    sel->active = false;
    sel->pending = false;
    sel->kind = GC_SEL_NONE;
}

void
ghostcon_selection_start(ghostcon_selection_t *sel, int16_t x, int16_t y,
                         ghostcon_sel_kind_t kind, uint16_t cols) {
    sel->x1 = x; sel->y1 = y;
    sel->x2 = x; sel->y2 = y;
    sel->cols = (int16_t)cols;
    sel->kind = kind;
    sel->active = true;
    sel->pending = true;
}

void
ghostcon_selection_update(ghostcon_selection_t *sel, int16_t x, int16_t y) {
    if (!sel->pending) return;
    sel->x2 = x;
    sel->y2 = y;
}

void
ghostcon_selection_finish(ghostcon_selection_t *sel) {
    sel->pending = false;
}

bool
ghostcon_selection_row_range(const ghostcon_selection_t *sel, int16_t y, int16_t cols,
                              int16_t *out_xstart, int16_t *out_xend) {
    if (!sel->active) return false;

    int16_t ymin = sel->y1 < sel->y2 ? sel->y1 : sel->y2;
    int16_t ymax = sel->y1 > sel->y2 ? sel->y1 : sel->y2;
    if (y < ymin || y > ymax) return false;

    int16_t xstart, xend;
    if (sel->kind == GC_SEL_RECT || ymin == ymax) {
        /* Rect selection, or a single-row char selection -- both are
           bounded by the drag's x-extent regardless of which endpoint
           came first. */
        xstart = sel->x1 < sel->x2 ? sel->x1 : sel->x2;
        xend   = sel->x1 > sel->x2 ? sel->x1 : sel->x2;
    } else {
        /* Multi-row char selection: y1/y2 aren't pre-sorted (a drag
           can go in any direction), so figure out which endpoint's x
           belongs to the TOP row vs the BOTTOM row first. */
        int16_t top_x    = (sel->y1 <= sel->y2) ? sel->x1 : sel->x2;
        int16_t bottom_x = (sel->y1 <= sel->y2) ? sel->x2 : sel->x1;
        if (y == ymin) { xstart = top_x; xend = cols - 1; }
        else if (y == ymax) { xstart = 0; xend = bottom_x; }
        else { xstart = 0; xend = cols - 1; }
    }
    if (xstart < 0) xstart = 0;
    if (xend >= cols) xend = cols - 1;
    if (xstart > xend) return false;

    *out_xstart = xstart;
    *out_xend = xend;
    return true;
}

bool
ghostcon_selection_contains(const ghostcon_selection_t *sel, int16_t x, int16_t y) {
    int16_t xstart, xend;
    if (!ghostcon_selection_row_range(sel, y, sel->cols, &xstart, &xend))
        return false;
    return x >= xstart && x <= xend;
}

/* Minimal UTF-8 encoder for a single codepoint -- no existing helper
   in the tree does this (glyph/atlas code works in codepoints, not
   UTF-8 bytes). Returns bytes written (1-4), or 0 if `out_len` is too
   small to fit the encoded codepoint. */
static size_t
utf8_encode(uint32_t cp, char *out, size_t out_len) {
    if (cp < 0x80) {
        if (out_len < 1) return 0;
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        if (out_len < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (out_len < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        if (out_len < 4) return 0;
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

size_t
ghostcon_selection_extract_text(ghostcon_screen_t *screen, char *out, size_t out_len) {
    const ghostcon_selection_t *sel = &screen->selection;
    if (!sel->active || out_len == 0) return 0;

    int16_t ymin = sel->y1 < sel->y2 ? sel->y1 : sel->y2;
    int16_t ymax = sel->y1 > sel->y2 ? sel->y1 : sel->y2;
    size_t oi = 0;

    for (int16_t y = ymin; y <= ymax && oi + 4 < out_len; y++) {
        ghostcon_row_t *row = ghostcon_screen_row(screen, (uint16_t)y);
        if (!row) continue;

        int16_t xstart, xend;
        if (!ghostcon_selection_row_range(sel, y, (int16_t)row->cols, &xstart, &xend))
            continue;

        size_t row_start_oi = oi;
        for (int16_t x = xstart; x <= xend && oi + 4 < out_len; x++) {
            ghostcon_cell_t cell = row->cells[x];
            if (ghostcon_cell_get_wide(cell) == GHOSTCON_CELL_WIDE_SPACER_TAIL)
                continue; /* second half of a wide glyph -- already emitted */
            /* ghostcon_cell_has_text() is NOT the right predicate here
               -- a totally blank, never-written cell still has tag
               GHOSTCON_CELL_CODEPOINT (0, the default), so has_text()
               returns true for it with codepoint 0, which would encode
               as a literal NUL byte instead of a space. Check both:
               is_empty() (c.raw == 0, "nothing was ever written") and
               an explicit codepoint-0 cell that DOES have the text tag
               set but was written with a NUL (defensive, same net
               effect either way -- treat as blank). */
            if (ghostcon_cell_is_empty(cell) || !ghostcon_cell_has_text(cell) ||
                ghostcon_cell_get_codepoint(cell) == 0) {
                out[oi++] = ' ';
                continue;
            }
            /* Combining/grapheme-extension codepoints beyond the
               cell's base one are dropped for this pass -- an
               accepted simplification, not a correctness requirement
               (see PLAN.md). */
            uint32_t cp = ghostcon_cell_get_codepoint(cell);
            size_t n = utf8_encode(cp, out + oi, out_len - oi);
            if (n == 0) break; /* wouldn't fit -- stop this row */
            oi += n;
        }
        while (oi > row_start_oi && out[oi - 1] == ' ')
            oi--; /* trim trailing spaces on this row */

        bool next_is_wrap_continuation = false;
        if (y < ymax) {
            ghostcon_row_t *next = ghostcon_screen_row(screen, (uint16_t)(y + 1));
            next_is_wrap_continuation = next && next->wrap_continuation;
        }
        if (y < ymax && !next_is_wrap_continuation && oi + 1 < out_len)
            out[oi++] = '\n';
    }
    out[oi] = '\0';
    return oi;
}
