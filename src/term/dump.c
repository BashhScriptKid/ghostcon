#include "ghostcon/term/dump.h"
#include "ghostcon/term/cell.h"

void
ghostcon_screen_dump(FILE *f, ghostcon_screen_t *s)
{
    fprintf(f, "cols=%u\n", s->cols);
    fprintf(f, "rows=%u\n", s->rows_visible);
    fprintf(f, "screen=%s\n", s->alt_screen_active ? "alt" : "primary");
    fprintf(f, "cursor_x=%d\n", (int)s->cursor.x);
    fprintf(f, "cursor_y=%d\n", (int)s->cursor.y);
    fprintf(f, "pending_wrap=%u\n", s->cursor.pending_wrap ? 1u : 0u);

    for (uint16_t y = 0; y < s->rows_visible; y++) {
        ghostcon_row_t *r = ghostcon_screen_row(s, y);
        fprintf(f, "row%uwrap=%u\n", y, (r && r->wrap) ? 1u : 0u);
    }

    for (uint16_t y = 0; y < s->rows_visible; y++) {
        ghostcon_row_t *r = ghostcon_screen_row(s, y);
        if (!r)
            continue;
        for (uint16_t x = 0; x < s->cols; x++) {
            ghostcon_cell_t c = r->cells[x];
            uint32_t cp = ghostcon_cell_get_codepoint(c);
            uint32_t wide = ghostcon_cell_get_wide(c);
            uint32_t prot = ghostcon_cell_get_protected(c) ? 1u : 0u;
            uint32_t styled = ghostcon_cell_get_style(c) != GC_STYLE_ID_DEFAULT ? 1u : 0u;
            if (cp == 0 && wide == 0 && prot == 0 && styled == 0)
                continue;
            fprintf(f, "cell %u %u cp=0x%06X wide=%u prot=%u styled=%u\n",
                    x, y, cp, wide, prot, styled);
        }
    }
}
