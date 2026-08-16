#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Procedural box-drawing / block-element rendering                    */
/*                                                                     */
/* Box-drawing (U+2500-254B), block-element (U+2580-259F), and legacy  */
/* computing sextant/octant (U+1FB00-1FBFF, U+1CC00+) characters are   */
/* meant to tile edge-to-edge across adjacent cells. Rasterizing them  */
/* from a font, like every other glyph, leaves whatever bearing/       */
/* padding the font happens to use around that glyph -- invisible for  */
/* ordinary letters, but a visible seam for shapes meant to connect    */
/* seamlessly (found live: a TUI splash logo built from these          */
/* characters rendered as a visible checkerboard of gaps, while        */
/* ordinary text in the same frame was crisp). Ghostty itself never    */
/* sources these from a font at all -- see its src/font/sprite/draw/,  */
/* which draws them procedurally, sized exactly to the cell. This is   */
/* the same idea, reimplemented here in C for ghostcon's own renderer. */
/* ------------------------------------------------------------------ */

/* Returns true if `codepoint` is one this module handles -- caller
   should bypass the font atlas entirely for it. On true, fills
   `alpha` (cell_w * cell_h bytes, row-major, one 0-255 coverage value
   per pixel) with the rasterized shape for the given cell size. */
bool ghostcon_box_draw_render(uint32_t codepoint, int cell_w, int cell_h,
                               uint8_t *alpha);
