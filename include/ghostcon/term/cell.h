#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Cell content tag (2 bits) */
typedef enum
{
    GC_CELL_CODEPOINT         = 0, /* single codepoint (or empty) */
    GC_CELL_CODEPOINT_GRAPHEME = 1, /* first cp of multi-cp grapheme */
    GC_CELL_BG_COLOR_PALETTE  = 2, /* no text, only bg from palette */
    GC_CELL_BG_COLOR_RGB      = 3, /* no text, only bg as RGB       */
} ghostcon_cell_content_tag_t;

/* Wide char status (2 bits) */
typedef enum
{
    GC_CELL_WIDE_NARROW      = 0,
    GC_CELL_WIDE_WIDE        = 1, /* width-2 char (followed by spacer_tail) */
    GC_CELL_WIDE_SPACER_TAIL = 2, /* second cell of wide char */
    GC_CELL_WIDE_SPACER_HEAD = 3, /* wide char wraps to next line */
} ghostcon_cell_wide_t;

/* Semantic content type (2 bits) */
typedef enum
{
    GC_CELL_SEMANTIC_OUTPUT = 0,
    GC_CELL_SEMANTIC_INPUT  = 1,
    GC_CELL_SEMANTIC_PROMPT = 2,
} ghostcon_cell_semantic_t;

/* RGB color */
typedef struct
{
    uint8_t r, g, b;
} ghostcon_rgb_t;

/* Cell: 8-byte packed representation (matching Ghostty's Cell)
 *
 * Memory layout (64 bits total):
 * bits  0- 1: content_tag
 * bits  2-25: content (u24: codepoint u21, or RGB, or palette idx)
 * bits 26-40: style_id (15 bits, 0 = default)
 * bits 41-42: wide (2 bits)
 * bit     43: protected
 * bit     44: (unused, formerly a 1-bit hyperlink flag -- replaced
 * by the 15-bit hyperlink_id field below since a bare
 * bool can't distinguish which hyperlink a cell is in)
 * bits 45-46: semantic_content (2 bits)
 * bits 47-61: hyperlink_id (15 bits, 0 = no hyperlink, same width
 * and 0-is-default convention as style_id)
 * bits 62-63: padding
 */
typedef struct
{
    uint64_t raw;
} ghostcon_cell_t;

/* Bitfield positions and widths.
 *
 * Every mask here is the *width* of its field, unshifted -- the same mask
 * serves the getter (which shifts down and masks) and the setter (which
 * masks and shifts up), so a getter and its setter cannot disagree about
 * how wide the field is.
 *
 * The content field is 24 bits wide and holds three different things,
 * each with its own width, so it has three masks:
 *
 *   codepoint   21 bits
 *   rgb         24 bits (the whole field -- GC_CELL_CONTENT_MASK)
 *   palette      8 bits
 */
#define GC_CELL_TAG_SHIFT        0
#define GC_CELL_TAG_MASK         0x3
#define GC_CELL_CONTENT_SHIFT    2
#define GC_CELL_CONTENT_MASK     0xFFFFFF
#define GC_CELL_CODEPOINT_MASK   0x1FFFFF
#define GC_CELL_PALETTE_MASK     0xFF
#define GC_CELL_STYLE_SHIFT      26
#define GC_CELL_STYLE_MASK       0x7FFF
#define GC_CELL_WIDE_SHIFT       41
#define GC_CELL_WIDE_MASK        0x3
#define GC_CELL_PROTECTED_SHIFT  43
#define GC_CELL_PROTECTED_MASK   0x1
#define GC_CELL_SEMANTIC_SHIFT   45
#define GC_CELL_SEMANTIC_MASK    0x3
#define GC_CELL_HYPERLINK_ID_SHIFT 47
#define GC_CELL_HYPERLINK_ID_MASK  0x7FFF

/* Generic access to the cell's simple bitfields.
 *
 * Everything except the 24-bit content field (which holds three different
 * things, and so has its own three accessors) goes through this pair.
 */
#define GC_CELL_GET(c, field) \
    ((uint32_t)(((c).raw >> GC_CELL_##field##_SHIFT) & GC_CELL_##field##_MASK))

#define GC_CELL_SET(c, field, v) \
    ((c).raw = ((c).raw & ~((uint64_t)GC_CELL_##field##_MASK << GC_CELL_##field##_SHIFT)) | \
               (((uint64_t)(v) & GC_CELL_##field##_MASK) << GC_CELL_##field##_SHIFT))

/* Style ID type (15-bit, 0 = default style)
 *
 * The default style's id is GC_STYLE_DEFAULT_ID, in style.h -- this header
 * only owns the type and its range, and style.h includes this one, so the
 * constant cannot live here without an include cycle.
 */
typedef uint16_t ghostcon_style_id_t;
#define GC_STYLE_ID_MAX     0x7FFF

/* Codepoint extracted from content field */
#define GC_CELL_MAX_CODEPOINT GC_CELL_CODEPOINT_MASK

/* Inline accessors */

static inline ghostcon_cell_content_tag_t
ghostcon_cell_get_tag(ghostcon_cell_t c)
{
    return (ghostcon_cell_content_tag_t)GC_CELL_GET(c, TAG);
}

static inline void ghostcon_cell_set_tag(ghostcon_cell_t *c, ghostcon_cell_content_tag_t tag)
{
    GC_CELL_SET(*c, TAG, tag);
}

static inline uint32_t ghostcon_cell_get_codepoint(ghostcon_cell_t c)
{
    return (uint32_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & GC_CELL_CODEPOINT_MASK);
}

static inline void ghostcon_cell_set_codepoint(ghostcon_cell_t *c, uint32_t cp)
{
    c->raw = (c->raw & ~(GC_CELL_CONTENT_MASK << GC_CELL_CONTENT_SHIFT)) |
             ((uint64_t)(cp & GC_CELL_CODEPOINT_MASK) << GC_CELL_CONTENT_SHIFT);
}

/* Content as RGB (only valid when tag == BG_COLOR_RGB) */
static inline ghostcon_rgb_t
ghostcon_cell_get_rgb(ghostcon_cell_t c)
{
    uint32_t v = (uint32_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & GC_CELL_CONTENT_MASK);
    return (ghostcon_rgb_t){
        .r = (uint8_t)((v >> 16) & 0xFF),
        .g = (uint8_t)((v >> 8) & 0xFF),
        .b = (uint8_t)(v & 0xFF),
    };
}

static inline void ghostcon_cell_set_rgb(ghostcon_cell_t *c, ghostcon_rgb_t rgb)
{
    uint32_t v = ((uint32_t)rgb.r << 16) | ((uint32_t)rgb.g << 8) | rgb.b;
    c->raw = (c->raw & ~(GC_CELL_CONTENT_MASK << GC_CELL_CONTENT_SHIFT)) |
             ((uint64_t)(v & GC_CELL_CONTENT_MASK) << GC_CELL_CONTENT_SHIFT);
}

/* Content as palette index (only valid when tag == BG_COLOR_PALETTE) */
static inline uint8_t ghostcon_cell_get_palette_idx(ghostcon_cell_t c)
{
    return (uint8_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & GC_CELL_PALETTE_MASK);
}

static inline void ghostcon_cell_set_palette_idx(ghostcon_cell_t *c, uint8_t idx)
{
    c->raw = (c->raw & ~(GC_CELL_CONTENT_MASK << GC_CELL_CONTENT_SHIFT)) |
             ((uint64_t)(idx & GC_CELL_PALETTE_MASK) << GC_CELL_CONTENT_SHIFT);
}

static inline ghostcon_style_id_t
ghostcon_cell_get_style(ghostcon_cell_t c)
{
    return (ghostcon_style_id_t)GC_CELL_GET(c, STYLE);
}

static inline void ghostcon_cell_set_style(ghostcon_cell_t *c, ghostcon_style_id_t id)
{
    GC_CELL_SET(*c, STYLE, id);
}

static inline ghostcon_cell_wide_t
ghostcon_cell_get_wide(ghostcon_cell_t c)
{
    return (ghostcon_cell_wide_t)GC_CELL_GET(c, WIDE);
}

static inline void ghostcon_cell_set_wide(ghostcon_cell_t *c, ghostcon_cell_wide_t w)
{
    GC_CELL_SET(*c, WIDE, w);
}

static inline bool ghostcon_cell_get_protected(ghostcon_cell_t c)
{
    return GC_CELL_GET(c, PROTECTED) != 0;
}

static inline void ghostcon_cell_set_protected(ghostcon_cell_t *c, bool v)
{
    GC_CELL_SET(*c, PROTECTED, v ? 1 : 0);
}

static inline ghostcon_style_id_t
ghostcon_cell_get_hyperlink_id(ghostcon_cell_t c)
{
    return (ghostcon_style_id_t)GC_CELL_GET(c, HYPERLINK_ID);
}

static inline void ghostcon_cell_set_hyperlink_id(ghostcon_cell_t *c, ghostcon_style_id_t id)
{
    GC_CELL_SET(*c, HYPERLINK_ID, id);
}

static inline bool ghostcon_cell_get_hyperlink(ghostcon_cell_t c)
{
    return ghostcon_cell_get_hyperlink_id(c) != 0;
}

static inline ghostcon_cell_semantic_t
ghostcon_cell_get_semantic(ghostcon_cell_t c)
{
    return (ghostcon_cell_semantic_t)GC_CELL_GET(c, SEMANTIC);
}

static inline void ghostcon_cell_set_semantic(ghostcon_cell_t *c, ghostcon_cell_semantic_t s)
{
    GC_CELL_SET(*c, SEMANTIC, s);
}

/* Utility predicates */
static inline bool ghostcon_cell_is_empty(ghostcon_cell_t c)
{
    return c.raw == 0;
}

static inline bool ghostcon_cell_has_text(ghostcon_cell_t c)
{
    ghostcon_cell_content_tag_t t = ghostcon_cell_get_tag(c);
    return t == GC_CELL_CODEPOINT ||
           t == GC_CELL_CODEPOINT_GRAPHEME;
}

static inline bool ghostcon_cell_has_grapheme(ghostcon_cell_t c)
{
    return ghostcon_cell_get_tag(c) == GC_CELL_CODEPOINT_GRAPHEME;
}

/* Grid width: 2 for wide/spacer_head cells, 1 otherwise */
static inline uint8_t ghostcon_cell_grid_width(ghostcon_cell_t c)
{
    switch (ghostcon_cell_get_wide(c))
    {
    case GC_CELL_WIDE_WIDE:
    case GC_CELL_WIDE_SPACER_HEAD:
        return 2;
    default:
        return 1;
    }
}

/* Initialize a zero cell */
static inline ghostcon_cell_t
ghostcon_cell_make_empty(void)
{
    ghostcon_cell_t c = { .raw = 0 };
    return c;
}

/* Initialize a cell with a codepoint */
static inline ghostcon_cell_t
ghostcon_cell_make(uint32_t codepoint)
{
    ghostcon_cell_t c = { .raw = 0 };
    ghostcon_cell_set_tag(&c, GC_CELL_CODEPOINT);
    ghostcon_cell_set_codepoint(&c, codepoint);
    return c;
}

/* Zero-initialized cell constant */
#define GC_CELL_EMPTY ((ghostcon_cell_t){ .raw = 0 })

/* Unicode display width: 0 (combining/zero-width), 1, or 2 (wide) */
extern uint8_t ghostcon_unicode_width(uint32_t codepoint);

/* Matches Ghostty's isSymbol() (renderer/cell.zig): Unicode General Category
 * "Symbol" (Sm, Sc, Sk, So), backed by a table generated from the UCD.
 *
 * The table is src/term/cell_symbol_table.gen.c, produced by
 * tools/gen_symbol_table.py from third_party/UnicodeData.txt. Ghostty uses the
 * same source, just generated at their build time.
 *
 * Two carve-outs override the generated data, because the UCD cannot express
 * them:
 *
 * - Box Drawing and Block Elements (U+2500-259F) are excluded despite
 *   being category So: render/box_draw.c draws them procedurally.
 * - The Private Use Area (U+E000-F8FF, where Nerd Font icons live) is
 *   included despite being category Co: those glyphs need the same
 *   treatment in practice.
 *
 * See ghostcon_cell_is_symbol() in cell.c for the exact logic.
 *
 * Why this exists: a symbol is correctly one cell wide according to
 * ghostcon_unicode_width() (verified live), but the installed font's glyph can
 * still rasterize wider than a cell.
 *
 * That is common for icon/dingbat glyphs, especially in Nerd Fonts.
 *
 * Observed live: a Nerd Font "pull request" icon, several Geometric
 * Shapes/Mathematical Operators circles (U+2299, U+25C9, U+25CE), and the
 * git-pull-request octicon (U+F407) all exceeded cell_w at the configured font
 * size.
 *
 * render/machine.c uses this to choose the context-aware fit-to-cell treatment
 * (see ghostcon_cell_is_graphics_element() below and machine.c's
 * symbol_constraint_width()) instead of drawing those glyphs at native size like
 * ordinary text.
 */
extern bool ghostcon_cell_is_symbol(uint32_t codepoint);

/* Codepoint ranges whose glyphs are specifically designed to tile/ connect
 * edge-to-edge across adjacent cells: Legacy Computing (sextants etc.) and
 * Powerline glyphs.
 *
 * Box-drawing (U+2500-257F) and block elements (U+2580-259F) are NOT included
 * here
 *
 * they're already rendered procedurally at exactly cell_w/cell_h by
 * render/box_draw.c and never reach the font-glyph path this distinction matters
 * for at all.
 *
 * Mirrors Ghostty's isGraphicsElement(): used only to exempt these from the
 * "previous cell was also a symbol -> constrain to 1 cell" rule in machine.c's
 * symbol_constraint_width(), since squeezing a tiling glyph down would visibly
 * break the tiling.
 */
extern bool ghostcon_cell_is_graphics_element(uint32_t codepoint);
