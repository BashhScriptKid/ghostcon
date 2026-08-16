#include "ghostcon/term/selection.h"

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
ghostcon_selection_contains(const ghostcon_selection_t *sel, int16_t x, int16_t y) {
    if (!sel->active) return false;

    int16_t xmin = sel->x1 < sel->x2 ? sel->x1 : sel->x2;
    int16_t xmax = sel->x1 > sel->x2 ? sel->x1 : sel->x2;
    int16_t ymin = sel->y1 < sel->y2 ? sel->y1 : sel->y2;
    int16_t ymax = sel->y1 > sel->y2 ? sel->y1 : sel->y2;

    if (sel->kind == GC_SEL_RECT) {
        return y >= ymin && y <= ymax && x >= xmin && x <= xmax;
    }

    if (y < ymin || y > ymax) return false;
    if (y > ymin && y < ymax) return true;
    if (y == ymin && x >= xmin) return true;
    if (y == ymax && x <= xmax) return true;
    return false;
}
