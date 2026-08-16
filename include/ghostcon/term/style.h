#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cell.h"

/* ------------------------------------------------------------------ */
/* Style — text attributes for a cell                                 */
/*                                                                     */
/* Each cell references a style via ghostcon_style_id_t (15-bit).      */
/* style_id 0 = default style (no attributes, default fg/bg colors).   */
/*                                                                     */
/* Style flags map 1:1 to Ghostty's style.zig attributes.             */
/* ------------------------------------------------------------------ */

#define GC_STYLE_DEFAULT_ID 0

/* Bitfield flags for style attributes */
typedef enum {
    GC_STYLE_BOLD              = (1 << 0),
    GC_STYLE_DIM               = (1 << 1),
    GC_STYLE_ITALIC            = (1 << 2),
    GC_STYLE_UNDERLINE         = (1 << 3),
    GC_STYLE_BLINK             = (1 << 4),
    GC_STYLE_INVERSE           = (1 << 5),
    GC_STYLE_HIDDEN            = (1 << 6),
    GC_STYLE_STRIKETHROUGH     = (1 << 7),
    GC_STYLE_OVERLINE          = (1 << 8),
    GC_STYLE_FG_TRUECOLOR      = (1 << 9),
    GC_STYLE_BG_TRUECOLOR      = (1 << 10),
    GC_STYLE_UNDERLINE_TRUECOLOR = (1 << 11),
    GC_STYLE_FG_DEFAULT          = (1 << 12), /* use terminal default fg (ignore fg_palette) */
    GC_STYLE_BG_DEFAULT          = (1 << 13), /* use terminal default bg (ignore bg_palette) */
} ghostcon_style_flag_t;

/* Underline style (stored separately from flags) */
typedef enum {
    GC_STYLE_UL_NONE,
    GC_STYLE_UL_SINGLE,
    GC_STYLE_UL_DOUBLE,
    GC_STYLE_UL_CURLY,
    GC_STYLE_UL_DOTTED,
    GC_STYLE_UL_DASHED,
} ghostcon_style_underline_t;

/* Full style descriptor */
typedef struct {
    uint16_t           flags;             /* bitmask of ghostcon_style_flag_t */
    uint8_t            fg_palette;        /* foreground palette index (0-255) */
    uint8_t            bg_palette;        /* background palette index (0-255) */
    ghostcon_rgb_t     fg_rgb;            /* foreground truecolor (when FG_TRUECOLOR) */
    ghostcon_rgb_t     bg_rgb;            /* background truecolor (when BG_TRUECOLOR) */
    ghostcon_rgb_t     ul_rgb;            /* underline color (when UNDERLINE_TRUECOLOR) */
    ghostcon_style_underline_t underline;  /* underline style */
    uint8_t            pad;              /* padding to 16 bytes */
} ghostcon_style_t;

/* Default style constant — empty, fg=default, bg=default */
extern const ghostcon_style_t GHOSTCON_STYLE_DEFAULT;

static inline bool ghostcon_style_eq(const ghostcon_style_t *a, const ghostcon_style_t *b) {
    return a->flags == b->flags &&
           a->fg_palette == b->fg_palette &&
           a->bg_palette == b->bg_palette &&
           a->fg_rgb.r == b->fg_rgb.r && a->fg_rgb.g == b->fg_rgb.g && a->fg_rgb.b == b->fg_rgb.b &&
           a->bg_rgb.r == b->bg_rgb.r && a->bg_rgb.g == b->bg_rgb.g && a->bg_rgb.b == b->bg_rgb.b &&
           a->ul_rgb.r == b->ul_rgb.r && a->ul_rgb.g == b->ul_rgb.g && a->ul_rgb.b == b->ul_rgb.b &&
           a->underline == b->underline;
}

/* ------------------------------------------------------------------ */
/* StyleSet — a ref-counted set of styles                             */
/*                                                                     */
/* Simplified version of Ghostty's StyleSet (RefCountedSet).           */
/* Maps ghostcon_style_id_t → ghostcon_style_t with reference counts. */
/* ------------------------------------------------------------------ */
typedef struct ghostcon_style_set ghostcon_style_set_t;

/* Create a style set with initial capacity */
ghostcon_style_set_t *ghostcon_style_set_create(uint16_t capacity);

/* Destroy a style set */
void ghostcon_style_set_destroy(ghostcon_style_set_t *set);

/* Add a style, returning its ID. If the style already exists, increments refcount. */
/* Returns GC_STYLE_DEFAULT_ID if style == default. */
ghostcon_style_id_t ghostcon_style_set_add(ghostcon_style_set_t *set, const ghostcon_style_t *style);

/* Increment refcount for an existing ID */
void ghostcon_style_set_ref(ghostcon_style_set_t *set, ghostcon_style_id_t id);

/* Decrement refcount; frees the style slot if refcount reaches 0 */
void ghostcon_style_set_unref(ghostcon_style_set_t *set, ghostcon_style_id_t id);

/* Get style by ID. Returns &GHOSTCON_STYLE_DEFAULT for id=0. */
const ghostcon_style_t *ghostcon_style_set_get(const ghostcon_style_set_t *set, ghostcon_style_id_t id);
