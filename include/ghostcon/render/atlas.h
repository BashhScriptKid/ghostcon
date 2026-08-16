#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Font glyph atlas                                                    */
/*                                                                     */
/* Fontconfig selects the font, freetype rasterizes each glyph on      */
/* first use, and glyphs are shelf-packed into a single 8-bit          */
/* (alpha-only) CPU-side bitmap. The renderer uploads/re-uploads this  */
/* bitmap to a GL_ALPHA texture whenever ghostcon_atlas_dirty() is     */
/* true. See PLAN.md "Renderer design" / IMPLEMENTATION.md "Font atlas"*/
/* ------------------------------------------------------------------ */

typedef struct {
    float    u0, v0, u1, v1;   /* UV rect in the atlas texture, normalized */
    int16_t  width, height;    /* glyph bitmap size, pixels */
    int16_t  bearing_x, bearing_y; /* pen-to-bitmap-top-left offset, pixels */
    float    advance;          /* horizontal advance, pixels */
} ghostcon_glyph_t;

typedef struct ghostcon_atlas ghostcon_atlas_t;

/* font_family may be NULL to use fontconfig's default monospace match.
   atlas_dim is the atlas bitmap's width/height in pixels (square,
   power-of-two recommended, e.g. 1024 or 2048). */
ghostcon_atlas_t *ghostcon_atlas_create(const char *font_family,
                                         int font_size_px,
                                         uint32_t atlas_dim);
void ghostcon_atlas_destroy(ghostcon_atlas_t *atlas);

/* Looks up the glyph for a codepoint, rasterizing and packing it into
   the atlas on first use. Returns NULL if the glyph has no outline
   (e.g. codepoint not in the font) or the atlas is full. */
const ghostcon_glyph_t *ghostcon_atlas_glyph(ghostcon_atlas_t *atlas,
                                              uint32_t codepoint);

/* CPU-side atlas bitmap: atlas_dim * atlas_dim bytes, one byte (alpha)
   per pixel — upload as GL_ALPHA / GL_LUMINANCE. */
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
