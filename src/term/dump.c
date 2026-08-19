#include "ghostcon/term/dump.h"
#include "ghostcon/term/cell.h"
#include <stdbool.h>

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
            ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(c);
            uint32_t cp = (tag == GHOSTCON_CELL_CODEPOINT || tag == GHOSTCON_CELL_CODEPOINT_GRAPHEME)
                ? ghostcon_cell_get_codepoint(c) : 0;
            uint32_t wide = ghostcon_cell_get_wide(c);
            uint32_t prot = ghostcon_cell_get_protected(c) ? 1u : 0u;
            ghostcon_style_id_t style_id = ghostcon_cell_get_style(c);
            uint32_t styled = style_id != GC_STYLE_ID_DEFAULT ? 1u : 0u;
            bool content_bg = (tag == GHOSTCON_CELL_BG_COLOR_PALETTE || tag == GHOSTCON_CELL_BG_COLOR_RGB);
            if (cp == 0 && wide == 0 && prot == 0 && styled == 0 && !content_bg)
                continue;

            uint32_t bg_default, bg_truecolor, bg_palette;
            uint8_t bg_r, bg_g, bg_b;
            if (tag == GHOSTCON_CELL_BG_COLOR_PALETTE) {
                bg_default = 0; bg_truecolor = 0;
                bg_palette = ghostcon_cell_get_palette_idx(c);
                bg_r = bg_g = bg_b = 0;
            } else if (tag == GHOSTCON_CELL_BG_COLOR_RGB) {
                bg_default = 0; bg_truecolor = 1; bg_palette = 0;
                ghostcon_rgb_t rgb = ghostcon_cell_get_rgb(c);
                bg_r = rgb.r; bg_g = rgb.g; bg_b = rgb.b;
            } else {
                const ghostcon_style_t *st = ghostcon_style_set_get(s->styles, style_id);
                bg_default = (st->flags & GC_STYLE_BG_DEFAULT) ? 1u : 0u;
                bg_truecolor = (st->flags & GC_STYLE_BG_TRUECOLOR) ? 1u : 0u;
                bg_palette = st->bg_palette;
                bg_r = st->bg_rgb.r; bg_g = st->bg_rgb.g; bg_b = st->bg_rgb.b;
            }
            fprintf(f, "cell %u %u cp=0x%06X wide=%u prot=%u styled=%u tag=%u "
                    "bg_default=%u bg_truecolor=%u bg_palette=%u bg_rgb=%02X%02X%02X\n",
                    x, y, cp, wide, prot, styled, (unsigned)tag,
                    bg_default, bg_truecolor, bg_palette, bg_r, bg_g, bg_b);
        }
    }
}
