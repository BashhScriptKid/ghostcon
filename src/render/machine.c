#include "ghostcon/render/machine.h"

#include <math.h>

#include "ghostcon/term/cell.h"
#include "ghostcon/term/style.h"
#include "ghostcon/term/color.h"

/* Port of Ghostty's renderer/cell.zig constraintWidth() -- how many
   cells (1 or 2) a "symbol"-classified glyph (see cell.h's own doc
   comment on ghostcon_cell_is_symbol()) is allowed to occupy this
   frame. Context-aware, not a fixed per-glyph property: a symbol at
   the screen edge, or immediately following another symbol (unless
   that one tiles across cells, e.g. Powerline), is squeezed to 1 cell;
   otherwise, if the NEXT cell is blank, it's allowed to borrow that
   space and render at its natural size across 2 cells. Only when
   neither applies does the caller need to actually scale the glyph
   down. */
static uint8_t
symbol_constraint_width(const ghostcon_row_t *row, uint16_t x)
{
    if (x == (uint16_t)(row->cols - 1))
        return 1; /* screen edge -- no room to borrow */

    if (x > 0) {
        uint32_t prev_cp = ghostcon_cell_get_codepoint(row->cells[x - 1]);
        if (ghostcon_cell_is_symbol(prev_cp) && !ghostcon_cell_is_graphics_element(prev_cp))
            return 1; /* keep adjacent symbol glyphs aligned, not overlapping */
    }

    uint32_t next_cp = ghostcon_cell_get_codepoint(row->cells[x + 1]);
    if (next_cp == 0 || next_cp == ' ')
        return 2; /* next cell is blank -- borrow its space */

    return 1;
}

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

            /* Figure out up front whether this cell's glyph (if any) is
               a symbol borrowing the next cell's space, so the
               background-quad push below can be adjusted accordingly.
               Real bug found live: the width-borrowing decision itself
               (symbol_constraint_width()) was correct, but the NEXT
               cell still got its own full background quad pushed when
               the loop reached it moments later -- submitted AFTER
               this glyph's draw call, so it painted right back over
               the portion that extended into it, undoing the fix
               entirely. Fixed by pushing BOTH cells' backgrounds here
               (each with its own resolved color, not assumed to
               match) and skipping the borrowed cell's own iteration
               entirely, rather than letting it redundantly repaint. */
            ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(cell);
            uint32_t cp = (tag == GHOSTCON_CELL_CODEPOINT ||
                           tag == GHOSTCON_CELL_CODEPOINT_GRAPHEME)
                ? ghostcon_cell_get_codepoint(cell) : 0;
            bool is_symbol = cp != 0 && cp != ' ' && ghostcon_cell_is_symbol(cp);
            uint8_t constraint_width = is_symbol ? symbol_constraint_width(row, x) : 1;

            ghostcon_gles_push_rect(gles, px, py, (float)cell_w, (float)cell_h,
                                     bg[0], bg[1], bg[2], 1.0f);

            if (constraint_width == 2) {
                ghostcon_cell_t next_cell = row->cells[x + 1];
                const ghostcon_style_t *next_style =
                    ghostcon_style_set_get(screen->styles, ghostcon_cell_get_style(next_cell));
                float next_fg[3], next_bg[3];
                resolve_colors(screen, next_style, next_fg, next_bg);
                ghostcon_gles_push_rect(gles, px + (float)cell_w, py, (float)cell_w, (float)cell_h,
                                         next_bg[0], next_bg[1], next_bg[2], 1.0f);
            }

            if (style->flags & GC_STYLE_HIDDEN) {
                if (constraint_width == 2)
                    x++;
                continue;
            }

            ghostcon_cell_wide_t wide = ghostcon_cell_get_wide(cell);
            if (wide == GHOSTCON_CELL_WIDE_SPACER_TAIL ||
                wide == GHOSTCON_CELL_WIDE_SPACER_HEAD)
                continue;

            if (cp == 0)
                continue;

            const ghostcon_glyph_t *glyph = ghostcon_atlas_glyph(atlas, cp);
            if (!glyph) {
                if (constraint_width == 2)
                    x++;
                continue;
            }

            /* Baseline-align: bearing_y is this glyph's offset from the
               baseline up to its bitmap top. The baseline itself must
               sit at a fixed offset from the cell top for every glyph
               on the line — the font's ascent metric — not cell_h;
               anchoring at cell_h leaves no room below the baseline,
               so descenders (g, q, y, p, j) spill into the row below
               and get overdrawn by its background quad. */
            float gx = px + (float)glyph->bearing_x;
            float gy = py + (float)ascent - (float)glyph->bearing_y;
            float draw_w = (float)glyph->width;
            float draw_h = (float)glyph->height;

            /* Symbol/icon glyphs (Nerd Font icons, Geometric Shapes,
               Dingbats, ...) are correctly single-width per
               ghostcon_unicode_width() -- confirmed matching the
               reference terminal exactly -- but the installed font's
               actual rasterized glyph can be physically wider than
               one cell regardless (found live: a Nerd Font
               git-pull-request icon and several circle/dot glyphs all
               measured wider than cell_w at the configured font size).
               Drawing those at native size left the right portion
               erased the instant the next cell's own background quad
               got pushed -- "half the glyph renders, half is cut off".
               Fix, ported from Ghostty's own symbol-constraint system:
               let it borrow the next cell's space if that cell is
               blank (symbol_constraint_width() above), and only if it
               STILL doesn't fit, scale it down (never up) to fit,
               centered in the resulting bounds. Ordinary text glyphs
               are completely untouched by any of this -- draw_w/draw_h
               stay at native size and gx/gy stay baseline-anchored,
               exactly as before. */
            if (is_symbol) {
                float bound_w = (float)(constraint_width * cell_w);
                float bound_h = (float)cell_h;
                float scale = 1.0f;
                if (draw_w > bound_w)
                    scale = bound_w / draw_w;
                if (draw_h * scale > bound_h)
                    scale = bound_h / draw_h;
                if (scale < 1.0f) {
                    draw_w *= scale;
                    draw_h *= scale;
                }
                /* Snap to a whole pixel -- a fractional destination Y
                   (e.g. padding of an odd pixel count centers at a
                   half-pixel offset) causes asymmetric texel sampling
                   between the quad's top and bottom edges: the source
                   bitmap ends up losing a row on one edge but not the
                   other, even though it's genuinely symmetric (found
                   live -- confirmed by dumping the raw FreeType bitmap
                   directly: perfectly symmetric top-to-bottom, so this
                   was never a font/rasterization issue). Floor, not
                   round, so the glyph never grows past its already-
                   established bound_w/bound_h on either edge. */
                gx = floorf(px + (bound_w - draw_w) / 2.0f);
                gy = floorf(py + (bound_h - draw_h) / 2.0f);
            }

            ghostcon_gles_push_glyph(gles, gx, gy, draw_w, draw_h,
                                      glyph, fg[0], fg[1], fg[2], 1.0f);

            if (constraint_width == 2)
                x++; /* skip the borrowed cell -- already background-painted above */
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
    /* While scrolled back into history (view_offset > 0, see screen.h's
       own doc comment), the grid coordinates the cursor was last left
       at are now showing spliced-in history content, not the live line
       the cursor actually belongs to -- drawing it there just looks
       like a stray mark sitting on the wrong text. Matches how most
       terminals handle this: hide the cursor while scrolled back,
       rather than drawing it somewhere misleading. */
    if (!screen->cursor_visible || screen->view_offset > 0)
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

/* Alpha for the selection tint -- low enough that the glyph/background
   color underneath still reads clearly, unlike GC_STYLE_INVERSE (which
   swaps colors outright rather than tinting). */
#define GC_SELECTION_ALPHA 0.35f

void
ghostcon_machine_render_selection(ghostcon_screen_t *screen,
                                   ghostcon_gles_t *gles,
                                   int cell_w, int cell_h)
{
    const ghostcon_selection_t *sel = &screen->selection;
    if (!sel->active || screen->view_offset > 0)
        return;

    int16_t ymin = sel->y1 < sel->y2 ? sel->y1 : sel->y2;
    int16_t ymax = sel->y1 > sel->y2 ? sel->y1 : sel->y2;

    GhosttyColorRgb c = screen->palette.cursor_color;
    float r = (float)c.r / 255.0f, g = (float)c.g / 255.0f, b = (float)c.b / 255.0f;

    for (int16_t y = ymin; y <= ymax && y < (int16_t)screen->rows_visible; y++) {
        if (y < 0)
            continue;
        int16_t xstart, xend;
        if (!ghostcon_selection_row_range(sel, y, (int16_t)screen->cols, &xstart, &xend))
            continue;

        float px = (float)(xstart * cell_w);
        float py = (float)(y * cell_h);
        float w = (float)((xend - xstart + 1) * cell_w);
        ghostcon_gles_push_rect(gles, px, py, w, (float)cell_h, r, g, b, GC_SELECTION_ALPHA);
    }
}
