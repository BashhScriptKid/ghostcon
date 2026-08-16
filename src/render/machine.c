#include "ghostcon/render/machine.h"

#include "ghostcon/term/cell.h"
#include "ghostcon/term/style.h"
#include "ghostcon/term/color.h"

static void
resolve_colors(const ghostcon_screen_t *screen, const ghostcon_style_t *style,
                float *fg, float *bg)
{
    GhosttyColorRgb fg_rgb, bg_rgb;

    if (style->flags & GC_STYLE_FG_TRUECOLOR)
        fg_rgb = (GhosttyColorRgb){ style->fg_rgb.r, style->fg_rgb.g, style->fg_rgb.b };
    else if (style->flags & GC_STYLE_FG_DEFAULT)
        fg_rgb = screen->palette.fg_default;
    else
        fg_rgb = ghostcon_palette_resolve(&screen->palette, style->fg_palette);

    if (style->flags & GC_STYLE_BG_TRUECOLOR)
        bg_rgb = (GhosttyColorRgb){ style->bg_rgb.r, style->bg_rgb.g, style->bg_rgb.b };
    else if (style->flags & GC_STYLE_BG_DEFAULT)
        bg_rgb = screen->palette.bg_default;
    else
        bg_rgb = ghostcon_palette_resolve(&screen->palette, style->bg_palette);

    if (style->flags & GC_STYLE_INVERSE) {
        GhosttyColorRgb tmp = fg_rgb;
        fg_rgb = bg_rgb;
        bg_rgb = tmp;
    }

    fg[0] = (float)fg_rgb.r / 255.0f;
    fg[1] = (float)fg_rgb.g / 255.0f;
    fg[2] = (float)fg_rgb.b / 255.0f;

    bg[0] = (float)bg_rgb.r / 255.0f;
    bg[1] = (float)bg_rgb.g / 255.0f;
    bg[2] = (float)bg_rgb.b / 255.0f;
}

void
ghostcon_machine_render_dirty(ghostcon_screen_t *screen,
                               ghostcon_atlas_t *atlas,
                               ghostcon_gles_t *gles,
                               int cell_w, int cell_h)
{
    ghostcon_dirty_region_t dirty = ghostcon_screen_get_dirty(screen);
    if (dirty.y_min < 0)
        return;

    int ascent = ghostcon_atlas_ascent(atlas);

    for (int16_t y = dirty.y_min; y <= dirty.y_max; y++) {
        ghostcon_row_t *row = ghostcon_screen_row(screen, (uint16_t)y);
        if (!row)
            continue;

        for (uint16_t x = 0; x < row->cols; x++) {
            ghostcon_cell_t cell = row->cells[x];
            const ghostcon_style_t *style =
                ghostcon_style_set_get(screen->styles, ghostcon_cell_get_style(cell));

            float fg[3], bg[3];
            resolve_colors(screen, style, fg, bg);

            float px = (float)(x * cell_w);
            float py = (float)(y * cell_h);

            ghostcon_gles_push_rect(gles, px, py, (float)cell_w, (float)cell_h,
                                     bg[0], bg[1], bg[2], 1.0f);

            if (style->flags & GC_STYLE_HIDDEN)
                continue;

            ghostcon_cell_wide_t wide = ghostcon_cell_get_wide(cell);
            if (wide == GHOSTCON_CELL_WIDE_SPACER_TAIL ||
                wide == GHOSTCON_CELL_WIDE_SPACER_HEAD)
                continue;

            ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(cell);
            if (tag != GHOSTCON_CELL_CODEPOINT &&
                tag != GHOSTCON_CELL_CODEPOINT_GRAPHEME)
                continue;

            uint32_t cp = ghostcon_cell_get_codepoint(cell);
            if (cp == 0 || cp == ' ')
                continue;

            const ghostcon_glyph_t *glyph = ghostcon_atlas_glyph(atlas, cp);
            if (!glyph)
                continue;

            /* Baseline-align: bearing_y is this glyph's offset from the
               baseline up to its bitmap top. The baseline itself must
               sit at a fixed offset from the cell top for every glyph
               on the line — the font's ascent metric — not cell_h;
               anchoring at cell_h leaves no room below the baseline,
               so descenders (g, q, y, p, j) spill into the row below
               and get overdrawn by its background quad. */
            float gx = px + (float)glyph->bearing_x;
            float gy = py + (float)ascent - (float)glyph->bearing_y;

            ghostcon_gles_push_glyph(gles, gx, gy,
                                      (float)glyph->width, (float)glyph->height,
                                      glyph, fg[0], fg[1], fg[2], 1.0f);
        }
    }
}

/* Thickness for the underline/bar cursor shapes, in pixels. Matches
   common terminal convention (a thin accent line, not a half-cell
   block) rather than any specific font metric. */
#define GC_CURSOR_LINE_THICKNESS 2

void
ghostcon_machine_render_cursor(ghostcon_screen_t *screen,
                                ghostcon_gles_t *gles,
                                int cell_w, int cell_h)
{
    if (!screen->cursor_visible)
        return;

    int16_t x = screen->cursor.x;
    int16_t y = screen->cursor.y;
    if (x < 0 || y < 0 || x >= (int16_t)screen->cols || y >= (int16_t)screen->rows_visible)
        return;

    GhosttyColorRgb c = screen->palette.cursor_color;
    float r = (float)c.r / 255.0f, g = (float)c.g / 255.0f, b = (float)c.b / 255.0f;

    float px = (float)(x * cell_w);
    float py = (float)(y * cell_h);

    switch (screen->cursor.cursor_style) {
    case GC_CURSOR_UNDERLINE:
    case GC_CURSOR_UNDERLINE_BLINK:
        ghostcon_gles_push_rect(gles, px, py + (float)(cell_h - GC_CURSOR_LINE_THICKNESS),
                                 (float)cell_w, (float)GC_CURSOR_LINE_THICKNESS,
                                 r, g, b, 1.0f);
        break;
    case GC_CURSOR_BAR:
    case GC_CURSOR_BAR_BLINK:
        ghostcon_gles_push_rect(gles, px, py,
                                 (float)GC_CURSOR_LINE_THICKNESS, (float)cell_h,
                                 r, g, b, 1.0f);
        break;
    case GC_CURSOR_DEFAULT:
    case GC_CURSOR_BLOCK:
    case GC_CURSOR_BLOCK_BLINK:
    default:
        ghostcon_gles_push_rect(gles, px, py, (float)cell_w, (float)cell_h,
                                 r, g, b, 1.0f);
        break;
    }
}
