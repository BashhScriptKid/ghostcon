#define _DEFAULT_SOURCE

#include "ghostcon/core/ui.h"
#include "ghostcon/core/log.h"
#include <stdio.h>
#include <string.h>

ghostcon_cursor_state_t
ghostcon_ui_compute_cursor_state(ghostcon_screen_t *screen,
                                 int cell_w, int cell_h,
                                 int pointer_x, int pointer_y)
{
    if (!screen || cell_w <= 0 || cell_h <= 0 || pointer_x < 0 || pointer_y < 0)
        return GC_CURSOR_STATE_DEFAULT;

    int col = pointer_x / cell_w;
    int row = pointer_y / cell_h;
    if (row < 0 || row >= (int)screen->rows_visible)
        return GC_CURSOR_STATE_DEFAULT;

    ghostcon_row_t *hover_row = ghostcon_screen_row(screen, (uint16_t)row);
    if (hover_row && col >= 0 && col < hover_row->cols &&
        ghostcon_cell_get_hyperlink(hover_row->cells[col]))
    {
        return GC_CURSOR_STATE_LINK;
    }

    return GC_CURSOR_STATE_DEFAULT;
}

bool
ghostcon_ui_update_selection(ghostcon_screen_t *screen,
                             const ghostcon_input_pointer_t *pointer,
                             int cell_w, int cell_h)
{
    if (!screen || !pointer || cell_w <= 0 || cell_h <= 0)
        return false;

    int16_t col = (int16_t)(pointer->x / cell_w);
    int16_t row = (int16_t)(pointer->y / cell_h);
    bool need_render = false;

    if (pointer->left_pressed) {
        ghostcon_selection_start(&screen->selection, col, row,
                                 GC_SEL_CHAR, screen->cols);
        need_render = true;
    }
    if (pointer->moved && screen->selection.pending) {
        ghostcon_selection_update(&screen->selection, col, row);
        need_render = true;
    }
    if (pointer->left_released && screen->selection.pending) {
        ghostcon_selection_finish(&screen->selection);
        ghostcon_selection_t *sel = &screen->selection;
        if (sel->x1 == sel->x2 && sel->y1 == sel->y2)
            ghostcon_selection_clear(sel);
        need_render = true;
    }

    return need_render;
}

int
ghostcon_ui_compute_zoom(int current_font_size, int zoom_delta, int zoom_step,
                         int min_font_size, int max_font_size)
{
    if (zoom_delta == 0)
        return current_font_size;

    int new_size = current_font_size + zoom_delta * zoom_step;
    if (new_size < min_font_size)
        new_size = min_font_size;
    if (new_size > max_font_size)
        new_size = max_font_size;

    return new_size;
}

void
ghostcon_ui_extract_command_token(const char *title, char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    if (!title || !*title) {
        out[0] = '\0';
        return;
    }

    size_t word_len = strcspn(title, " \t");
    const char *word_end = title + word_len;
    const char *slash = NULL;
    for (const char *p = title; p < word_end; p++) {
        if (*p == '/')
            slash = p;
    }
    const char *start = slash ? slash + 1 : title;
    size_t n = (size_t)(word_end - start);
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

void
ghostcon_ui_build_comm_name(int vt_num, const char *token, char *out)
{
    if (!out)
        return;

    char prefix[16];
    int prefix_len = snprintf(prefix, sizeof(prefix), "gc@tty%d ", vt_num);
    if (prefix_len < 0) prefix_len = 0;
    if (prefix_len > 14) prefix_len = 14;

    int budget = 14 - prefix_len;
    size_t token_len = token ? strlen(token) : 0;

    if (budget <= 0) {
        char combined[128];
        snprintf(combined, sizeof(combined), "%.*s%s", prefix_len, prefix, token ? token : "");
        size_t n = strlen(combined);
        if (n > 14) n = 14;
        memcpy(out, combined, n);
        out[n] = '\0';
        return;
    }

    if ((int)token_len <= budget) {
        snprintf(out, 16, "%.*s%s", prefix_len, prefix, token ? token : "");
        return;
    }

    int keep = budget - 3;
    if (keep <= 0) {
        char combined[128];
        snprintf(combined, sizeof(combined), "%.*s%s", prefix_len, prefix, token ? token : "");
        size_t n = strlen(combined);
        if (n > 14) n = 14;
        memcpy(out, combined, n);
        out[n] = '\0';
        return;
    }

    snprintf(out, 16, "%.*s...%s", prefix_len, prefix, token + (token_len - (size_t)keep));
}
