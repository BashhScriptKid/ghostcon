#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Cell content tag (2 bits)                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    GHOSTCON_CELL_CODEPOINT         = 0, /* single codepoint (or empty) */
    GHOSTCON_CELL_CODEPOINT_GRAPHEME = 1, /* first cp of multi-cp grapheme */
    GHOSTCON_CELL_BG_COLOR_PALETTE  = 2, /* no text, only bg from palette */
    GHOSTCON_CELL_BG_COLOR_RGB      = 3, /* no text, only bg as RGB       */
} ghostcon_cell_content_tag_t;

/* Wide char status (2 bits) */
typedef enum {
    GHOSTCON_CELL_WIDE_NARROW      = 0,
    GHOSTCON_CELL_WIDE_WIDE        = 1, /* width-2 char (followed by spacer_tail) */
    GHOSTCON_CELL_WIDE_SPACER_TAIL = 2, /* second cell of wide char */
    GHOSTCON_CELL_WIDE_SPACER_HEAD = 3, /* wide char wraps to next line */
} ghostcon_cell_wide_t;

/* Semantic content type (2 bits) */
typedef enum {
    GHOSTCON_CELL_SEMANTIC_OUTPUT = 0,
    GHOSTCON_CELL_SEMANTIC_INPUT  = 1,
    GHOSTCON_CELL_SEMANTIC_PROMPT = 2,
} ghostcon_cell_semantic_t;

/* RGB color */
typedef struct {
    uint8_t r, g, b;
} ghostcon_rgb_t;

/* ------------------------------------------------------------------ */
/* Cell: 8-byte packed representation (matching Ghostty's Cell)       */
/*                                                                     */
/* Memory layout (64 bits total):                                      */
/*   bits  0- 1: content_tag                                          */
/*   bits  2-25: content (u24: codepoint u21, or RGB, or palette idx) */
/*   bits 26-40: style_id (15 bits, 0 = default)                      */
/*   bits 41-42: wide (2 bits)                                        */
/*   bit     43: protected                                            */
/*   bit     44: (unused, formerly a 1-bit hyperlink flag -- replaced  */
/*                by the 15-bit hyperlink_id field below since a bare  */
/*                bool can't distinguish which hyperlink a cell is in) */
/*   bits 45-46: semantic_content (2 bits)                             */
/*   bits 47-61: hyperlink_id (15 bits, 0 = no hyperlink, same width   */
/*                and 0-is-default convention as style_id)             */
/*   bits 62-63: padding                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t raw;
} ghostcon_cell_t;

/* Bitfield positions */
#define GC_CELL_TAG_SHIFT        0
#define GC_CELL_TAG_MASK         ((uint64_t)0x3 << 0)
#define GC_CELL_CONTENT_SHIFT    2
#define GC_CELL_CONTENT_MASK     ((uint64_t)0xFFFFFF << 2)
#define GC_CELL_STYLE_SHIFT      26
#define GC_CELL_STYLE_MASK       ((uint64_t)0x7FFF << 26)
#define GC_CELL_WIDE_SHIFT       41
#define GC_CELL_WIDE_MASK        ((uint64_t)0x3 << 41)
#define GC_CELL_PROTECTED_SHIFT  43
#define GC_CELL_PROTECTED_MASK   ((uint64_t)0x1 << 43)
#define GC_CELL_SEMANTIC_SHIFT   45
#define GC_CELL_SEMANTIC_MASK    ((uint64_t)0x3 << 45)
#define GC_CELL_HYPERLINK_ID_SHIFT 47
#define GC_CELL_HYPERLINK_ID_MASK  ((uint64_t)0x7FFF << 47)

/* Style ID type (15-bit, 0 = default style) */
typedef uint16_t ghostcon_style_id_t;
#define GC_STYLE_ID_DEFAULT 0
#define GC_STYLE_ID_MAX     0x7FFF

/* Codepoint extracted from content field */
#define GC_CELL_MAX_CODEPOINT 0x1FFFFF

/* ------------------------------------------------------------------ */
/* Inline accessors                                                    */
/* ------------------------------------------------------------------ */

static inline ghostcon_cell_content_tag_t
ghostcon_cell_get_tag(ghostcon_cell_t c) {
    return (ghostcon_cell_content_tag_t)((c.raw >> GC_CELL_TAG_SHIFT) & 0x3);
}

static inline void
ghostcon_cell_set_tag(ghostcon_cell_t *c, ghostcon_cell_content_tag_t tag) {
    c->raw = (c->raw & ~GC_CELL_TAG_MASK) | ((uint64_t)tag << GC_CELL_TAG_SHIFT);
}

static inline uint32_t
ghostcon_cell_get_codepoint(ghostcon_cell_t c) {
    return (uint32_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & 0x1FFFFF);
}

static inline void
ghostcon_cell_set_codepoint(ghostcon_cell_t *c, uint32_t cp) {
    c->raw = (c->raw & ~GC_CELL_CONTENT_MASK) |
             ((uint64_t)(cp & 0x1FFFFF) << GC_CELL_CONTENT_SHIFT);
}

/* Content as RGB (only valid when tag == BG_COLOR_RGB) */
static inline ghostcon_rgb_t
ghostcon_cell_get_rgb(ghostcon_cell_t c) {
    uint32_t v = (uint32_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & 0xFFFFFF);
    return (ghostcon_rgb_t){
        .r = (uint8_t)((v >> 16) & 0xFF),
        .g = (uint8_t)((v >> 8) & 0xFF),
        .b = (uint8_t)(v & 0xFF),
    };
}

static inline void
ghostcon_cell_set_rgb(ghostcon_cell_t *c, ghostcon_rgb_t rgb) {
    uint32_t v = ((uint32_t)rgb.r << 16) | ((uint32_t)rgb.g << 8) | rgb.b;
    c->raw = (c->raw & ~GC_CELL_CONTENT_MASK) |
             ((uint64_t)(v & 0xFFFFFF) << GC_CELL_CONTENT_SHIFT);
}

/* Content as palette index (only valid when tag == BG_COLOR_PALETTE) */
static inline uint8_t
ghostcon_cell_get_palette_idx(ghostcon_cell_t c) {
    return (uint8_t)((c.raw >> GC_CELL_CONTENT_SHIFT) & 0xFF);
}

static inline void
ghostcon_cell_set_palette_idx(ghostcon_cell_t *c, uint8_t idx) {
    c->raw = (c->raw & ~GC_CELL_CONTENT_MASK) |
             ((uint64_t)idx << GC_CELL_CONTENT_SHIFT);
}

static inline ghostcon_style_id_t
ghostcon_cell_get_style(ghostcon_cell_t c) {
    return (ghostcon_style_id_t)((c.raw >> GC_CELL_STYLE_SHIFT) & 0x7FFF);
}

static inline void
ghostcon_cell_set_style(ghostcon_cell_t *c, ghostcon_style_id_t id) {
    c->raw = (c->raw & ~GC_CELL_STYLE_MASK) |
             ((uint64_t)(id & 0x7FFF) << GC_CELL_STYLE_SHIFT);
}

static inline ghostcon_cell_wide_t
ghostcon_cell_get_wide(ghostcon_cell_t c) {
    return (ghostcon_cell_wide_t)((c.raw >> GC_CELL_WIDE_SHIFT) & 0x3);
}

static inline void
ghostcon_cell_set_wide(ghostcon_cell_t *c, ghostcon_cell_wide_t w) {
    c->raw = (c->raw & ~GC_CELL_WIDE_MASK) |
             ((uint64_t)w << GC_CELL_WIDE_SHIFT);
}

static inline bool
ghostcon_cell_get_protected(ghostcon_cell_t c) {
    return (bool)((c.raw >> GC_CELL_PROTECTED_SHIFT) & 0x1);
}

static inline void
ghostcon_cell_set_protected(ghostcon_cell_t *c, bool v) {
    c->raw = (c->raw & ~GC_CELL_PROTECTED_MASK) |
             ((uint64_t)(v ? 1 : 0) << GC_CELL_PROTECTED_SHIFT);
}

static inline ghostcon_style_id_t
ghostcon_cell_get_hyperlink_id(ghostcon_cell_t c) {
    return (ghostcon_style_id_t)((c.raw >> GC_CELL_HYPERLINK_ID_SHIFT) & 0x7FFF);
}

static inline void
ghostcon_cell_set_hyperlink_id(ghostcon_cell_t *c, ghostcon_style_id_t id) {
    c->raw = (c->raw & ~GC_CELL_HYPERLINK_ID_MASK) |
             ((uint64_t)(id & 0x7FFF) << GC_CELL_HYPERLINK_ID_SHIFT);
}

static inline bool
ghostcon_cell_get_hyperlink(ghostcon_cell_t c) {
    return ghostcon_cell_get_hyperlink_id(c) != 0;
}

static inline ghostcon_cell_semantic_t
ghostcon_cell_get_semantic(ghostcon_cell_t c) {
    return (ghostcon_cell_semantic_t)((c.raw >> GC_CELL_SEMANTIC_SHIFT) & 0x3);
}

static inline void
ghostcon_cell_set_semantic(ghostcon_cell_t *c, ghostcon_cell_semantic_t s) {
    c->raw = (c->raw & ~GC_CELL_SEMANTIC_MASK) |
             ((uint64_t)s << GC_CELL_SEMANTIC_SHIFT);
}

/* Utility predicates */
static inline bool ghostcon_cell_is_empty(ghostcon_cell_t c) {
    return c.raw == 0;
}

static inline bool ghostcon_cell_has_text(ghostcon_cell_t c) {
    ghostcon_cell_content_tag_t t = ghostcon_cell_get_tag(c);
    return t == GHOSTCON_CELL_CODEPOINT ||
           t == GHOSTCON_CELL_CODEPOINT_GRAPHEME;
}

static inline bool ghostcon_cell_has_grapheme(ghostcon_cell_t c) {
    return ghostcon_cell_get_tag(c) == GHOSTCON_CELL_CODEPOINT_GRAPHEME;
}

/* Grid width: 2 for wide/spacer_head cells, 1 otherwise */
static inline uint8_t ghostcon_cell_grid_width(ghostcon_cell_t c) {
    switch (ghostcon_cell_get_wide(c)) {
    case GHOSTCON_CELL_WIDE_WIDE:
    case GHOSTCON_CELL_WIDE_SPACER_HEAD:
        return 2;
    default:
        return 1;
    }
}

/* Initialize a zero cell */
static inline ghostcon_cell_t ghostcon_cell_make_empty(void) {
    ghostcon_cell_t c = { .raw = 0 };
    return c;
}

/* Initialize a cell with a codepoint */
static inline ghostcon_cell_t ghostcon_cell_make(uint32_t codepoint) {
    ghostcon_cell_t c = { .raw = 0 };
    ghostcon_cell_set_tag(&c, GHOSTCON_CELL_CODEPOINT);
    ghostcon_cell_set_codepoint(&c, codepoint);
    return c;
}

/* Zero-initialized cell constant */
#define GHOSTCON_CELL_EMPTY ((ghostcon_cell_t){ .raw = 0 })

/* Unicode display width: 0 (combining/zero-width), 1, or 2 (wide) */
uint8_t ghostcon_unicode_width(uint32_t codepoint);
