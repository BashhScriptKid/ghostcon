#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Font glyph atlas
 *
 * Fontconfig selects the font, freetype rasterizes each glyph on first use, and
 * glyphs are shelf-packed into a single RGB (3 bytes/ pixel) CPU-side bitmap.
 *
 * The renderer uploads/re-uploads this bitmap to a GL_RGB texture whenever
 * ghostcon_atlas_dirty() is true.
 *
 * Every mode except "cleartype" stores R=G=B (a plain coverage mask, replicated
 * across channels)
 *
 * only cleartype stores real distinct per-subpixel coverage.
 *
 * See PLAN.md "Renderer design" / IMPLEMENTATION.md "Font atlas", and
 * render/gles.c's FRAG_SRC doc comment for how the shader turns that mask into a
 * blended pixel.
 *
 * Color glyphs (CBDT/CBLC bitmap-strike emoji fonts, e.g. Noto Color Emoji) are a
 * SEPARATE second atlas (RGBA, 4 bytes/pixel), not the mask atlas above
 *
 * their pixels are real source color to sample directly, not a coverage mask to
 * tint with the cell's foreground color, so they can't share the mask atlas's
 * format or the shader path that consumes it.
 *
 * ghostcon_glyph_t.is_color says which atlas a given glyph's UV rect lives in.
 *
 * See ghostcon_atlas_color_bitmap() below and gles.c's
 * u_atlas_color/v_is_color_glyph.
 */

typedef struct
{
    /* UV rect, normalized. In the color atlas (ghostcon_atlas_color_bitmap()) if
     * is_color, otherwise in the mask atlas (ghostcon_atlas_bitmap())
     */
    float    u0, v0, u1, v1;
    int16_t  width, height;        /* glyph bitmap size, pixels */
    int16_t  bearing_x, bearing_y; /* pen-to-bitmap-top-left offset, pixels */
    float    advance;              /* horizontal advance, pixels */
    bool     is_color;             /* true for a CBDT/CBLC color bitmap glyph --
                                      see this header's own doc comment above */
} ghostcon_glyph_t;

typedef struct ghostcon_atlas ghostcon_atlas_t;

/* font_family: NULL selects fontconfig's default monospace match.
 *
 * font_variant: NULL or empty selects fontconfig's default style for the family.
 * Otherwise it is matched against FC_STYLE (for example "Bold", "Light", "Medium
 * Italic")
 *
 * run `fc-list <family>` to see what a family offers.
 *
 * antialiasing:
 *
 * - NULL or "grayscale": FreeType's normal rasterization target, the
 *   only mode that existed before this parameter did.
 * - "subpixel": FT_LOAD_TARGET_LCD, but not true per-channel subpixel
 *   blending. The three LCD subchannels are averaged into a replicated
 *   mask, so the result is grayscale with LCD-optimized hinting.
 * - "cleartype": true per-subpixel coverage, stored and blended
 *   per-channel. See gles.c's FRAG_SRC.
 * - "none": FT_LOAD_TARGET_MONO, no antialiasing at all.
 *
 * Unrecognized values fall back to "grayscale"; they are not errors.
 *
 * subpixel_order: used only by "cleartype". NULL or "rgb" matches the common
 * laptop panel; "bgr" is the alternative.
 *
 * It must match the display's physical left-to-right subpixel order, or cleartype
 * adds visible color fringing instead of removing it.
 *
 * atlas_dim: atlas width and height in pixels. Square and power-of-two is
 * recommended (for example 1024 or 2048).
 */
ghostcon_atlas_t *ghostcon_atlas_create(const char *font_family,
                                         const char *font_variant,
                                         const char *antialiasing,
                                         const char *subpixel_order,
                                         int font_size_px,
                                         uint32_t atlas_dim);
void ghostcon_atlas_destroy(ghostcon_atlas_t *atlas);

/* Looks up the glyph for a codepoint, rasterizing and packing it into the atlas
 * on first use.
 *
 * If the primary font has no outline for this codepoint, falls through
 * fontconfig's own system fallback chain for the same font-matching pattern
 * (lazily opening each candidate font only as needed) before giving up --found
 * live: without this, a codepoint the primary font simply doesn't cover (common
 * for emoji and Nerd Font icons) rendered as nothing at all, even when another
 * installed font on the system did have it.
 *
 * Returns NULL only if no font in that chain has an outline for it either, or the
 * atlas is full.
 */
const ghostcon_glyph_t *ghostcon_atlas_glyph(ghostcon_atlas_t *atlas,
                                              uint32_t codepoint);

/* Looks up a glyph by its raw FreeType glyph index in the atlas's PRIMARY
 * face, rather than by codepoint.
 *
 * Rasterizes and packs on first use[1].
 *
 * Needed for ligature glyphs: a shaped ligature, for example the single
 * glyph a font substitutes for "->", has no Unicode codepoint of its own
 * to look up by. HarfBuzz hands back a glyph index instead.
 *
 * See ghostcon_atlas_shape_run() below and render/ligature.c, the only
 * caller.
 *
 * Does NOT consult the fallback font chain[2].
 *
 * [1] Same as ghostcon_atlas_glyph() above.
 *
 * [2] Ligature shaping only ever runs against the primary face's own
 *     hb_font_t.
 */
const ghostcon_glyph_t *ghostcon_atlas_glyph_by_index(ghostcon_atlas_t *atlas,
                                                        uint32_t glyph_index);

/* Shapes `count` codepoints (a candidate ligature-trigger run, e.g. the three
 * codepoints of "->") through HarfBuzz using the atlas's primary font, writing up
 * to max_out resulting glyph indices/advances (pixels) into
 * out_glyph_indices/out_advances.
 *
 * Returns the number of output glyphs written, or -1 if the atlas has no HarfBuzz
 * font (harfbuzz wasn't found at build time).
 *
 * The caller (render/ligature.c) is responsible for deciding whether this was a
 * REAL ligature substitution
 *
 * i.e. return value < count, meaning HarfBuzz actually collapsed multiple input
 * codepoints into fewer output glyphs, not just count independent glyphs that
 * happen to sit adjacently.
 */
int ghostcon_atlas_shape_run(ghostcon_atlas_t *atlas, const uint32_t *codepoints, int count,
                              uint32_t *out_glyph_indices, float *out_advances, int max_out);

/* The PRIMARY face's glyph index for `codepoint` with no shaping applied --i.e.
 * what ghostcon_atlas_glyph(atlas, codepoint) would have drawn. render/ligature.c
 * compares this against ghostcon_atlas_shape_run()'s output to tell whether
 * HarfBuzz actually substituted a contextual-alternate/ligature glyph for a given
 * cell, or just handed back the same glyph shaping always would have.
 *
 * Returns 0 (FreeType's own "no glyph" sentinel) if the primary face has no
 * outline for codepoint at all.
 */
uint32_t ghostcon_atlas_plain_glyph_index(ghostcon_atlas_t *atlas, uint32_t codepoint);

/* CPU-side atlas bitmap: atlas_dim * atlas_dim * 3 bytes, RGB (one byte per
 * channel) per pixel
 *
 * upload as GL_RGB.
 */
const uint8_t *ghostcon_atlas_bitmap(const ghostcon_atlas_t *atlas);
uint32_t ghostcon_atlas_dim(const ghostcon_atlas_t *atlas);

/* True if the bitmap changed since the last ghostcon_atlas_clear_dirty()
 * call (i.e. the GPU texture needs re-uploading).
 */
bool ghostcon_atlas_dirty(const ghostcon_atlas_t *atlas);
void ghostcon_atlas_clear_dirty(ghostcon_atlas_t *atlas);

/* CPU-side COLOR atlas bitmap: atlas_dim * atlas_dim * 4 bytes, RGBA (straight,
 * not premultiplied
 *
 * see atlas.c's un-premultiply step) per pixel
 *
 * upload as GL_RGBA.
 *
 * Shares the same dim as the mask atlas (ghostcon_atlas_dim() above covers both),
 * but is otherwise a fully separate bitmap/packer/dirty flag
 *
 * see this header's own doc comment on ghostcon_glyph_t.is_color for why.
 */
const uint8_t *ghostcon_atlas_color_bitmap(const ghostcon_atlas_t *atlas);
bool ghostcon_atlas_color_dirty(const ghostcon_atlas_t *atlas);
void ghostcon_atlas_color_clear_dirty(ghostcon_atlas_t *atlas);

/* Monospace cell size derived from the loaded font's metrics. */
void ghostcon_atlas_cell_size(const ghostcon_atlas_t *atlas,
                               int *cell_w, int *cell_h);

/* Baseline offset from a cell's top edge, pixels
 *
 * the font's ascender metric, not any per-glyph value.
 *
 * Use this (not cell_h) to position glyph quads so descenders (g, q, y, p, j)
 * land inside the cell instead of spilling into the row below.
 */
int ghostcon_atlas_ascent(const ghostcon_atlas_t *atlas);
