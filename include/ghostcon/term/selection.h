#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cell.h"

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
