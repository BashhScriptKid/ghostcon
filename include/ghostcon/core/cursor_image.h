#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Cursor image decoding — BMP and Xcursor, no DRM/KMS dependency       */
/*                                                                     */
/* Pure functions, directly unit-testable against synthetic files      */
/* (mirrors term/mouse.c's own split between encoding logic and the    */
/* real hardware-backed dispatch loop). See PLAN.md's "Cursor sprite:  */
/* raster images, per-state, config-driven" section for the full       */
/* design and format references.                                       */
/* ------------------------------------------------------------------ */

/* Decodes an uncompressed 24- or 32-bit BMP into a freshly malloc'd
   ARGB8888 buffer (row-major, top-to-bottom regardless of the BMP's
   own top-down/bottom-up storage order — that's normalized here).
   Caller frees the returned buffer. NULL on failure: missing file,
   or a BMP variant this deliberately minimal decoder doesn't attempt
   (RLE/compressed, paletted, 16-bit, anything but 24/32-bit BI_RGB).
   BMP carries no hotspot metadata, so callers of this function should
   treat the hotspot as (0,0). */
uint32_t *ghostcon_cursor_load_bmp(const char *path, uint32_t *out_w, uint32_t *out_h);

/* Writes `pixels` (ARGB8888, row-major, w*h entries) as an
   uncompressed 32-bit BMP to `path`. Returns false on an open/write
   failure. Symmetric with ghostcon_cursor_load_bmp() — round-trips
   losslessly (alpha channel included, unlike most BMP viewers'
   default handling, since this reader/writer pair always treats byte
   3 of each pixel as alpha). */
bool ghostcon_cursor_write_bmp(const char *path, const uint32_t *pixels,
                                uint32_t w, uint32_t h);

/* Reads one named cursor image (e.g. "text", "hand2") out of an
   Xcursor theme directory. `theme_dir` may be either the theme root
   (this function looks for a cursors/ subdirectory) or the cursors/
   directory itself (checked first, so a name that happens to collide
   with "cursors" as a literal cursor name — vanishingly unlikely —
   would need the root-dir form to disambiguate; not a real concern in
   practice). If the named file bundles multiple sizes (common, Xcursor
   themes often ship several for different DPIs), picks the LARGEST
   available. Returns a freshly malloc'd ARGB8888 buffer (caller
   frees), or NULL if the theme dir or the named file doesn't exist,
   or parsing fails (not a valid Xcursor file, or no image chunks in
   it). *out_hot_x and *out_hot_y are set from the file's own hotspot
   metadata for the size that was picked. */
uint32_t *ghostcon_cursor_load_xcursor(const char *theme_dir, const char *name,
                                        uint32_t *out_w, uint32_t *out_h,
                                        uint32_t *out_hot_x, uint32_t *out_hot_y);

/* Resizes an ARGB8888 buffer from src_w x src_h to dst_w x dst_h using
   nearest-neighbor sampling -- not bilinear: a cursor sprite is small
   enough (tens of pixels) that interpolation smoothing isn't worth the
   extra code, and nearest-neighbor keeps hard pixel-art edges crisp,
   which most cursor themes are drawn as. Returns a freshly malloc'd
   buffer (caller frees), or NULL if any dimension is 0 or allocation
   fails. dst_w == src_w && dst_h == src_h still allocates and copies
   (no identity short-circuit) -- callers needing to skip a pointless
   copy check that themselves. */
uint32_t *ghostcon_cursor_scale(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                                 uint32_t dst_w, uint32_t dst_h);

/* Crops `src` (ARGB8888, src_w x src_h) down to the tightest rectangle
   containing every non-transparent (alpha != 0) pixel -- real cursor
   assets (particularly ones pulled from an Xcursor theme, which often
   ship generous transparent padding around the glyph for shadow/
   antialiasing headroom) can be mostly empty space, which otherwise
   throws off both size (base_scale ends up scaling the padding, not
   the glyph) and position (the visible glyph sits well inside the
   asset's own bounding box, not at its (0,0) -- see PLAN.md). *hot_x/
   *hot_y are adjusted in place (shifted by the crop's own top-left, so
   they stay pointing at the same logical pixel in the now-smaller
   image; clamped to the cropped bounds if the original hotspot fell
   inside the trimmed padding). Returns a freshly malloc'd buffer
   (caller frees) and updates *out_w and *out_h, or NULL if `src` is fully
   transparent (nothing to crop to -- caller should keep the original
   image as-is in that case) or allocation fails. */
uint32_t *ghostcon_cursor_crop_to_content(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                                           uint32_t *out_w, uint32_t *out_h,
                                           uint32_t *hot_x, uint32_t *hot_y);
