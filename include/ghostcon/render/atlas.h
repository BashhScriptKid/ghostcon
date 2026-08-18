#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Font glyph atlas                                                    */
/*                                                                     */
/* Fontconfig selects the font, freetype rasterizes each glyph on      */
/* first use, and glyphs are shelf-packed into a single RGB (3 bytes/  */
/* pixel) CPU-side bitmap. The renderer uploads/re-uploads this bitmap */
/* to a GL_RGB texture whenever ghostcon_atlas_dirty() is true. Every  */
/* mode except "cleartype" stores R=G=B (a plain coverage mask,        */
/* replicated across channels) -- only cleartype stores real distinct */
/* per-subpixel coverage. See PLAN.md "Renderer design" /              */
/* IMPLEMENTATION.md "Font atlas", and render/gles.c's FRAG_SRC doc    */
/* comment for how the shader turns that mask into a blended pixel.    */
/* ------------------------------------------------------------------ */

typedef struct {
    float    u0, v0, u1, v1;   /* UV rect in the atlas texture, normalized */
    int16_t  width, height;    /* glyph bitmap size, pixels */
    int16_t  bearing_x, bearing_y; /* pen-to-bitmap-top-left offset, pixels */
    float    advance;          /* horizontal advance, pixels */
} ghostcon_glyph_t;

typedef struct ghostcon_atlas ghostcon_atlas_t;

/* font_family may be NULL to use fontconfig's default monospace match.
   font_variant may be NULL/empty to use fontconfig's default style
   for that family -- otherwise matched against FC_STYLE (e.g. "Bold",
   "Light", "Medium Italic"; run `fc-list <family>` to see what a
   given family actually offers). antialiasing may be NULL/"grayscale"
   (FreeType's normal rasterization target, the only mode that existed
   before this parameter did), "subpixel" (FT_LOAD_TARGET_LCD, but
   NOT true RGB-subpixel-blended rendering -- the 3 LCD subchannels
   are averaged down to a plain replicated mask, same as grayscale
   just with LCD-optimized hinting), "cleartype" (true per-subpixel
   coverage stored and blended per-channel -- see gles.c's FRAG_SRC),
   or "none" (FT_LOAD_TARGET_MONO, no antialiasing at all); an
   unrecognized value falls back to "grayscale", not an error.
   subpixel_order matters only for "cleartype": NULL/"rgb" (the
   common laptop-panel layout) or "bgr", matching the physical
   left-to-right subpixel order of the actual display -- get this
   wrong and cleartype produces visible color fringing instead of
   removing it. atlas_dim is the atlas bitmap's width/height in pixels
   (square, power-of-two recommended, e.g. 1024 or 2048). */
ghostcon_atlas_t *ghostcon_atlas_create(const char *font_family,
                                         const char *font_variant,
                                         const char *antialiasing,
                                         const char *subpixel_order,
                                         int font_size_px,
                                         uint32_t atlas_dim);
void ghostcon_atlas_destroy(ghostcon_atlas_t *atlas);

/* Looks up the glyph for a codepoint, rasterizing and packing it into
   the atlas on first use. If the primary font has no outline for this
   codepoint, falls through fontconfig's own system fallback chain for
   the same font-matching pattern (lazily opening each candidate font
   only as needed) before giving up -- found live: without this, a
   codepoint the primary font simply doesn't cover (common for emoji
   and Nerd Font icons) rendered as nothing at all, even when another
   installed font on the system did have it. Returns NULL only if no
   font in that chain has an outline for it either, or the atlas is
   full. */
const ghostcon_glyph_t *ghostcon_atlas_glyph(ghostcon_atlas_t *atlas,
                                              uint32_t codepoint);

/* CPU-side atlas bitmap: atlas_dim * atlas_dim * 3 bytes, RGB (one
   byte per channel) per pixel — upload as GL_RGB. */
const uint8_t *ghostcon_atlas_bitmap(const ghostcon_atlas_t *atlas);
uint32_t ghostcon_atlas_dim(const ghostcon_atlas_t *atlas);

/* True if the bitmap changed since the last ghostcon_atlas_clear_dirty()
   call (i.e. the GPU texture needs re-uploading). */
bool ghostcon_atlas_dirty(const ghostcon_atlas_t *atlas);
void ghostcon_atlas_clear_dirty(ghostcon_atlas_t *atlas);

/* Monospace cell size derived from the loaded font's metrics. */
void ghostcon_atlas_cell_size(const ghostcon_atlas_t *atlas,
                               int *cell_w, int *cell_h);

/* Baseline offset from a cell's top edge, pixels — the font's ascender
   metric, not any per-glyph value. Use this (not cell_h) to position
   glyph quads so descenders (g, q, y, p, j) land inside the cell
   instead of spilling into the row below. */
int ghostcon_atlas_ascent(const ghostcon_atlas_t *atlas);
