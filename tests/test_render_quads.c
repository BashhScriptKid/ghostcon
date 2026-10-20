/* test_render_quads
 *
 * The render loop's decisions, checked without a GPU.
 *
 * ghostcon_machine_render_dirty() emits its quads through the GLES layer, so this
 * test stubs the two push calls out and checks what was pushed:
 *
 *   - the background layer: exactly one quad per cell, in cell order, at the
 *     cell's position, carrying the cell's resolved background colour;
 *   - the glyph layer: exactly one quad per cell that draws something, and
 *     none for a cell whose space a neighbouring symbol borrowed.
 *
 * The expectations are computed from the grid and from the same public APIs the
 * renderer itself uses (the atlas' glyph lookup, the cell's symbol test, the
 * palette), so the test does not depend on the installed font: a codepoint the
 * font has no glyph for produces no quad on either side, and the cell's
 * background colour comes out of the palette the screen carries.
 *
 * This is the module's first automated coverage.
 *
 * The render path had a GPU smoke test (tests/test_render.c, which SKIPs without
 * a render node) and nothing else, so the decisions that decide *what* is drawn
 * -- which cell borrows which, which cell draws nothing, which background is
 * under which glyph -- were only ever checked by looking at the screen.
 */

#include "ghostcon/render/atlas.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/cell.h"
#include "ghostcon/term/color.h"
#include "ghostcon/term/screen.h"
#include "ghostcon/term/style.h"
#include "ghostcon/term/term.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* --- the recorded quads --- */

#define MAX_QUADS 8192

typedef struct
{
    float x, y, w, h;
    float r, g, b, a;
    float bg_r, bg_g, bg_b;
} quad_t;

static quad_t bg_quads[MAX_QUADS];
static size_t bg_count;
static quad_t glyph_quads[MAX_QUADS];
static size_t glyph_count;

/* --- the GLES stubs
 *
 * These are the same functions render/gles.c defines
 *
 * the test links the render sources *without* gles.c, so these are the only
 * definitions.
 */

void ghostcon_gles_push_rect(ghostcon_gles_t *gles, float x, float y, float w, float h, float r, float g, float b, float a)
{
    (void)gles; (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a;
}

void ghostcon_gles_push_bg_rect(ghostcon_gles_t *gles, float x, float y, float w, float h, float r, float g, float b, float a)
{
    (void)gles;
    if (bg_count >= MAX_QUADS)
        return;
    bg_quads[bg_count++] = (quad_t){ x, y, w, h, r, g, b, a, 0, 0, 0 };
}

void ghostcon_gles_push_glyph(ghostcon_gles_t *gles, float x, float y, float w, float h, const ghostcon_glyph_t *glyph, float r, float g, float b, float a, float bg_r, float bg_g, float bg_b)
{
    (void)gles; (void)glyph;
    if (glyph_count >= MAX_QUADS)
        return;
    glyph_quads[glyph_count++] = (quad_t){ x, y, w, h, r, g, b, a, bg_r, bg_g, bg_b };
}

ghostcon_gles_image_t
*ghostcon_gles_kitty_tex_get(ghostcon_gles_t *gles, const struct ghostcon_kitty_graphics *kg, uint32_t image_id, uint32_t generation, const uint8_t *pixels, int width, int height, int bpp)
{
    (void)gles; (void)kg; (void)image_id; (void)generation; (void)pixels; (void)width; (void)height; (void)bpp;
    return NULL;
}

ghostcon_gles_image_t
*ghostcon_gles_sixel_tex_get(ghostcon_gles_t *gles, const struct ghostcon_sixel_state *sx, int slot, uint32_t generation, const uint8_t *pixels, int width, int height)
{
    (void)gles; (void)sx; (void)slot; (void)generation; (void)pixels; (void)width; (void)height;
    return NULL;
}

void ghostcon_gles_queue_image(ghostcon_gles_t *gles, ghostcon_gles_image_t *img, float x, float y, float w, float h, float src_x, float src_y, float src_w, float src_h, float alpha, bool above_text)
{
    (void)gles; (void)img; (void)x; (void)y; (void)w; (void)h;
    (void)src_x; (void)src_y; (void)src_w; (void)src_h; (void)alpha; (void)above_text;
}

/* --- the model
 *
 * What the renderer is expected to do, spelled out from the rules rather
 * than from the renderer's own code.
 */

static uint32_t model_codepoint(ghostcon_cell_t cell)
{
    ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(cell);
    return (tag == GC_CELL_CODEPOINT || tag == GC_CELL_CODEPOINT_GRAPHEME)
        ? ghostcon_cell_get_codepoint(cell) : 0;
}

/* The cell's resolved foreground and background colours.
 *
 * A cell carrying a background colour in its content (the erase-colour
 * representation) overrides the style's background entirely.
 */
static void model_colors(const ghostcon_screen_t *screen, ghostcon_cell_t cell, float *fg, float *bg)
{
    const ghostcon_style_t *style =
        ghostcon_style_set_get(screen->styles, ghostcon_cell_get_style(cell));

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

    if (style->flags & GC_STYLE_INVERSE)
    {
        GhosttyColorRgb tmp = fg_rgb;
        fg_rgb = bg_rgb;
        bg_rgb = tmp;
    }

    ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(cell);
    if (tag == GC_CELL_BG_COLOR_PALETTE)
        bg_rgb = ghostcon_palette_resolve(&screen->palette, ghostcon_cell_get_palette_idx(cell));
    else if (tag == GC_CELL_BG_COLOR_RGB)
    {
        ghostcon_rgb_t raw = ghostcon_cell_get_rgb(cell);
        bg_rgb = (GhosttyColorRgb){ raw.r, raw.g, raw.b };
    }

    fg[0] = (float)fg_rgb.r / 255.0f;
    fg[1] = (float)fg_rgb.g / 255.0f;
    fg[2] = (float)fg_rgb.b / 255.0f;
    bg[0] = (float)bg_rgb.r / 255.0f;
    bg[1] = (float)bg_rgb.g / 255.0f;
    bg[2] = (float)bg_rgb.b / 255.0f;
}

/* True when the cell's symbol borrows the next cell's space.
 *
 * The rules: a symbol borrows the cell to its right when that cell is
 * blank, except at the screen edge, and except when the cell to the left is
 * itself a symbol (adjacent symbols stay aligned rather than overlap).
 */
static bool model_borrows_next(const ghostcon_row_t *row, uint16_t x)
{
    if (x + 1 >= row->cols)
        return false; /* screen edge -- no room to borrow */

    uint32_t cp = model_codepoint(row->cells[x]);
    if (cp == 0 || cp == ' ' || !ghostcon_cell_is_symbol(cp))
        return false;

    if (x > 0)
    {
        uint32_t prev_cp = model_codepoint(row->cells[x - 1]);
        if (ghostcon_cell_is_symbol(prev_cp) && !ghostcon_cell_is_graphics_element(prev_cp))
            return false;
    }

    uint32_t next_cp = model_codepoint(row->cells[x + 1]);
    return next_cp == 0 || next_cp == ' ';
}

/* True when the cell draws a glyph.
 *
 * A cell draws nothing when it is hidden, when it is the second half of a
 * wide character, when it holds no codepoint, when the font has no glyph
 * for its codepoint, or when the cell to its left borrowed its space.
 */
static bool model_draws_glyph(const ghostcon_screen_t *screen, ghostcon_atlas_t *atlas, const ghostcon_row_t *row, uint16_t x, bool borrowed_by_left)
{
    if (borrowed_by_left)
        return false;

    ghostcon_cell_t cell = row->cells[x];
    const ghostcon_style_t *style =
        ghostcon_style_set_get(screen->styles, ghostcon_cell_get_style(cell));
    if (style->flags & GC_STYLE_HIDDEN)
        return false;

    ghostcon_cell_wide_t wide = ghostcon_cell_get_wide(cell);
    if (wide == GC_CELL_WIDE_SPACER_TAIL || wide == GC_CELL_WIDE_SPACER_HEAD)
        return false;

    uint32_t cp = model_codepoint(cell);
    if (cp == 0)
        return false;

    return ghostcon_atlas_glyph(atlas, cp) != NULL;
}

/* --- the checks --- */

static bool float_eq(float a, float b)
{
    float d = a - b;
    if (d < 0)
        d = -d;
    return d < 0.0005f;
}

static bool quad_color_eq(const quad_t *q, const float *rgb)
{
    return float_eq(q->r, rgb[0]) && float_eq(q->g, rgb[1]) && float_eq(q->b, rgb[2]);
}

/* Renders `screen` and checks the two layers against the model.
 */
static void check_render(ghostcon_screen_t *screen, ghostcon_atlas_t *atlas, int cell_w, int cell_h, const char *label)
{
    bg_count = 0;
    glyph_count = 0;
    ghostcon_machine_render_dirty(screen, atlas, NULL, cell_w, cell_h, false);

    /* --- the background layer: one quad per cell, in cell order --- */
    size_t expected_bg_count = 0;
    bool bg_ok = true;
    char msg[256];
    for (int16_t y = 0; y < (int16_t)screen->rows_visible; y++)
    {
        ghostcon_row_t *row = ghostcon_screen_row(screen, (uint16_t)y);
        if (!row)
            continue;
        for (uint16_t x = 0; x < row->cols; x++)
        {
            if (expected_bg_count >= bg_count)
            {
                bg_ok = false;
                break;
            }
            const quad_t *q = &bg_quads[expected_bg_count];
            float fg[3], bg[3];
            model_colors(screen, row->cells[x], fg, bg);
            if (!float_eq(q->x, (float)(x * cell_w)) || !float_eq(q->y, (float)(y * cell_h)) ||
                !float_eq(q->w, (float)cell_w) || !float_eq(q->h, (float)cell_h) ||
                !quad_color_eq(q, bg))
            {
                bg_ok = false;
                break;
            }
            expected_bg_count++;
        }
        if (!bg_ok)
            break;
    }
    snprintf(msg, sizeof msg, "%s: the background layer is one quad per cell, in order, with the cell's colour", label);
    CHECK(bg_ok && expected_bg_count == bg_count, msg);

    /* --- the glyph layer: one quad per cell that draws something --- */
    size_t expected_glyph_count = 0;
    bool glyph_ok = true;
    bool colors_ok = true;
    bool bounds_ok = true;
    for (int16_t y = 0; y < (int16_t)screen->rows_visible; y++)
    {
        ghostcon_row_t *row = ghostcon_screen_row(screen, (uint16_t)y);
        if (!row)
            continue;
        for (uint16_t x = 0; x < row->cols; x++)
        {
            bool borrowed_by_left = x > 0 && model_borrows_next(row, (uint16_t)(x - 1));
            if (!model_draws_glyph(screen, atlas, row, x, borrowed_by_left))
                continue;
            if (expected_glyph_count >= glyph_count)
            {
                glyph_ok = false;
                break;
            }
            const quad_t *q = &glyph_quads[expected_glyph_count];
            /* The background colour the glyph carries must be the colour of
             * the background under it -- that is what the shader's
             * per-channel blending assumes (see gles.c's own doc comment on
             * the v_bg term).
             */
            float fg[3], bg[3];
            model_colors(screen, row->cells[x], fg, bg);
            if (!float_eq(q->bg_r, bg[0]) || !float_eq(q->bg_g, bg[1]) || !float_eq(q->bg_b, bg[2]))
                colors_ok = false;

            /* A symbol is drawn inside a bound of one cell -- or two when it
             * borrows the space of the cell to its right -- and is centred in
             * that bound.
             *
             * The bound is the only observable effect the borrow has: the
             * cell it borrows is blank, so it draws no glyph either way, and
             * its background quad lands in the same place whoever pushes it.
             */
            uint32_t cp = model_codepoint(row->cells[x]);
            if (cp != 0 && cp != ' ' && ghostcon_cell_is_symbol(cp))
            {
                bool borrows = model_borrows_next(row, x);
                float bound_w = (float)(borrows ? 2 : 1) * cell_w;
                float bound_centre = (float)(x * cell_w) + bound_w / 2.0f;
                float drawn_centre = q->x + q->w / 2.0f;
                if (fabsf(drawn_centre - bound_centre) > 1.0f)
                    bounds_ok = false;
            }
            expected_glyph_count++;
        }
        if (!glyph_ok)
            break;
    }
    snprintf(msg, sizeof msg, "%s: the glyph layer is one quad per cell that draws something", label);
    CHECK(glyph_ok && expected_glyph_count == glyph_count, msg);
    snprintf(msg, sizeof msg, "%s: every glyph carries the background colour under it", label);
    CHECK(colors_ok, msg);
    snprintf(msg, sizeof msg, "%s: every symbol is drawn inside its bound (one cell, or two when it borrows)", label);
    CHECK(bounds_ok, msg);
}

/* A codepoint the font can actually render, and that the terminal treats as
 * a symbol (so the borrow rule applies to it).
 *
 * Which symbols a font has varies -- an emoji is in no monospace font, and a
 * Nerd Font icon is in no ordinary one -- so the test looks for one it can
 * use rather than insisting on a particular codepoint.
 */
static uint32_t find_symbol(ghostcon_atlas_t *atlas)
{
    static const uint32_t candidates[] = {
        0x2714, /* heavy check mark */
        0x2705, /* white heavy check mark */
        0x2605, /* black star */
        0x25CF, /* black circle */
        0x25B2, /* black up-pointing triangle */
        0x25A0, /* black square */
        0x26A1, /* high voltage sign */
        0x2708, /* airplane */
        0x2192, /* rightwards arrow */
        0x00A9, /* copyright sign */
        0x00AE, /* registered sign */
        0x2122, /* trade mark sign */
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
    {
        if (!ghostcon_cell_is_symbol(candidates[i]))
            continue;
        if (!ghostcon_atlas_glyph(atlas, candidates[i]))
            continue;
        return candidates[i];
    }
    return 0;
}

/* A tiny string builder, for the test's content. */
typedef struct
{
    char   buf[128];
    size_t len;
} sbuf_t;

static void sb_init(sbuf_t *sb)
{
    sb->len = 0;
    sb->buf[0] = '\0';
}

static void sb_putc(sbuf_t *sb, char c)
{
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(sbuf_t *sb, const char *s)
{
    while (*s)
        sb_putc(sb, *s++);
}

/* Appends a codepoint as UTF-8. */
static void sb_putcp(sbuf_t *sb, uint32_t cp)
{
    if (cp < 0x80)
    {
        sb_putc(sb, (char)cp);
    }
    else if (cp < 0x800)
    {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Feeds bytes to the terminal, the way a real terminal receives them.
 *
 * The term layer decodes the UTF-8, so a multi-byte codepoint lands in the
 * screen as one cell (feeding the screen's put_char() byte by byte would
 * not).
 */
static void feed(ghostcon_term_t *term, const char *text)
{
    ghostcon_term_feed(term, (const uint8_t *)text, strlen(text));
}

int main(void)
{
    ghostcon_atlas_t *atlas = ghostcon_atlas_create(NULL, NULL, NULL, NULL, 16, 512);
    if (!atlas)
    {
        fprintf(stderr, "FAIL: ghostcon_atlas_create\n");
        return 1;
    }

    const int cell_w = 10, cell_h = 20;

    /* --- plain text --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "hello");
        check_render(&term.screen, atlas, cell_w, cell_h, "plain text");
        ghostcon_term_deinit(&term);
    }

    /* --- the symbol cases ---
     *
     * These need a symbol the installed font can render; without one there
     * is nothing to check, and the test says so rather than passing
     * silently.
     */
    uint32_t symbol = find_symbol(atlas);
    if (symbol == 0)
    {
        printf("SKIP: the symbol cases -- the installed font has no symbol glyph to draw\n");
    }
    else
    {
        printf("note: using U+%04X as the test symbol\n", symbol);

        /* a symbol next to a blank cell borrows its space */
        {
            sbuf_t sb;
            sb_init(&sb);
            sb_puts(&sb, "a ");
            sb_putcp(&sb, symbol);
            sb_putc(&sb, ' ');
            sb_putc(&sb, 'b');
            ghostcon_term_t term;
            ghostcon_term_init(&term, 20, 3, 0);
            feed(&term, sb.buf);
            check_render(&term.screen, atlas, cell_w, cell_h, "a symbol borrowing a blank cell");
            ghostcon_term_deinit(&term);
        }

        /* a symbol next to a non-blank cell borrows nothing */
        {
            sbuf_t sb;
            sb_init(&sb);
            sb_puts(&sb, "a ");
            sb_putcp(&sb, symbol);
            sb_putc(&sb, 'X');
            sb_putc(&sb, ' ');
            sb_putc(&sb, 'b');
            ghostcon_term_t term;
            ghostcon_term_init(&term, 20, 3, 0);
            feed(&term, sb.buf);
            check_render(&term.screen, atlas, cell_w, cell_h, "a symbol next to a non-blank cell");
            ghostcon_term_deinit(&term);
        }

        /* two adjacent symbols: the second borrows nothing */
        {
            sbuf_t sb;
            sb_init(&sb);
            sb_putcp(&sb, symbol);
            sb_putcp(&sb, symbol);
            sb_putc(&sb, ' ');
            sb_putc(&sb, 'x');
            ghostcon_term_t term;
            ghostcon_term_init(&term, 20, 3, 0);
            feed(&term, sb.buf);
            check_render(&term.screen, atlas, cell_w, cell_h, "two adjacent symbols");
            ghostcon_term_deinit(&term);
        }
    }

    /* --- a wide character and its spacer --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "a \xE4\xB8\xAD\xE6\x96\x87 b");
        check_render(&term.screen, atlas, cell_w, cell_h, "a wide character");
        ghostcon_term_deinit(&term);
    }

    /* --- a styled cell: the background colour comes from the style --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "plain ");
        /* A red background, as an SGR 41 sets it. */
        feed(&term, "\x1b[41m");
        feed(&term, "red");
        check_render(&term.screen, atlas, cell_w, cell_h, "a styled cell");
        ghostcon_term_deinit(&term);
    }

    /* --- a wide character that wraps to the next line --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        /* Fill the row, then put a wide character in the last column: it cannot
         * fit, so it wraps, and the cell it leaves behind carries the wide
         * character's codepoint as a "spacer head" -- which must not be drawn
         * (the character itself is drawn on the next line).
         */
        char filler[32];
        memset(filler, 'a', 19);
        filler[19] = '\0';
        feed(&term, filler);
        feed(&term, "\xE4\xB8\xAD");
        check_render(&term.screen, atlas, cell_w, cell_h, "a wide character that wraps");
        ghostcon_term_deinit(&term);
    }

    /* --- a hidden run draws nothing --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "a ");
        feed(&term, "\x1b[8m");  /* SGR 8: conceal */
        feed(&term, "hidden");
        feed(&term, "\x1b[28m"); /* SGR 28: reveal */
        feed(&term, " b");
        check_render(&term.screen, atlas, cell_w, cell_h, "a hidden run");
        ghostcon_term_deinit(&term);
    }

    /* --- a blank cell carrying a background colour in its content --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "x");
        ghostcon_screen_t *screen = &term.screen;
        /* The erase-colour representation: a blank cell whose content field
         * carries the background colour instead of a style id.
         */
        ghostcon_row_t *row = ghostcon_screen_row(screen, 0);
        ghostcon_cell_t cell = row->cells[1];
        ghostcon_cell_set_tag(&cell, GC_CELL_BG_COLOR_RGB);
        ghostcon_cell_set_rgb(&cell, (ghostcon_rgb_t){ 0x11, 0x22, 0x33 });
        row->cells[1] = cell;
        check_render(screen, atlas, cell_w, cell_h, "a cell with a content-tag background");
        ghostcon_term_deinit(&term);
    }

    /* --- the ligature path must not change the backgrounds --- */
    {
        ghostcon_term_t term;
        ghostcon_term_init(&term, 20, 3, 0);
        feed(&term, "a -> b != c");
        bg_count = 0;
        glyph_count = 0;
        ghostcon_machine_render_dirty(&term.screen, atlas, NULL, cell_w, cell_h, false);
        size_t bg_plain = bg_count;
        size_t glyph_plain = glyph_count;

        bg_count = 0;
        glyph_count = 0;
        ghostcon_machine_render_dirty(&term.screen, atlas, NULL, cell_w, cell_h, true);
        CHECK(bg_count == bg_plain,
              "ligatures: the background layer is the same with and without ligature shaping");
        CHECK(glyph_count <= glyph_plain,
              "ligatures: a ligature never draws more glyphs than the plain run");
        ghostcon_term_deinit(&term);
    }

    ghostcon_atlas_destroy(atlas);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
