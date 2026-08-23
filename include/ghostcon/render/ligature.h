#pragma once

#include <stdint.h>

#include "ghostcon/term/screen.h"
#include "ghostcon/render/atlas.h"

/* ------------------------------------------------------------------ */
/* Programming-ligature detection (opt-in, config.h's `ligatures`)     */
/*                                                                     */
/* ghostcon renders strictly one glyph per cell everywhere in the      */
/* pipeline, and ligatures here DON'T break that invariant -- found    */
/* live: monospace ligature fonts (confirmed against the actual        */
/* installed JetBrainsMono Nerd Font Mono) don't implement "->"/"=="/  */
/* etc. by collapsing multiple codepoints into one glyph spanning      */
/* several cells (that would break the fixed grid a monospace font     */
/* depends on). They implement it via `calt` CONTEXTUAL ALTERNATES:    */
/* each character individually gets substituted for a differently-     */
/* shaped glyph (e.g. '-' becomes "left half of an arrow shaft") that   */
/* happens to visually connect with its shaped neighbor -- still       */
/* exactly one glyph in its own single cell, just not the glyph a      */
/* plain codepoint lookup would have picked. So this module reports    */
/* PER-CELL glyph-index overrides, not multi-cell runs: machine.c just */
/* swaps which glyph it draws in an affected cell, with no change to   */
/* cell width, background-quad, or positioning logic at all.           */
/*                                                                     */
/* Only maximal runs of a fixed ASCII "ligature trigger" charset (the  */
/* operator/punctuation characters actual ligature fonts key calt off  */
/* of) are shaped, not arbitrary text -- see this file's own doc       */
/* comment on ghostcon_ligature_scan_row() for exactly what's shaped   */
/* and what's skipped. All HarfBuzz calls live in render/atlas.c       */
/* (ghostcon_atlas_shape_run/ghostcon_atlas_plain_glyph_index/          */
/* ghostcon_atlas_glyph_by_index) -- this file only decides which runs */
/* are worth shaping and interprets the result.                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t x;            /* cell this override applies to, within the row */
    uint32_t glyph_index;  /* substituted glyph -- look up via
                               ghostcon_atlas_glyph_by_index() */
    /* The full trigger-charset run this cell came from (e.g. both cells
       of "->", even though only the second one usually gets a non-empty
       override -- see machine.c's own doc comment on why). Some fonts
       (confirmed live: JetBrains Mono) draw a whole multi-cell ligature
       via ONE glyph with a bearing/width that intentionally spans the
       run rather than one glyph per cell each independently sized --
       machine.c uses [run_start, run_start+run_len) as the safe bound a
       substituted glyph's footprint is allowed to occupy, falling back
       to the plain per-codepoint glyph if it doesn't fit. */
    uint16_t run_start;
    uint16_t run_len;
} ghostcon_ligature_glyph_t;

/* Longest single run this module will attempt to shape, in cells.
   Generous headroom over any real ligature sequence (longest common
   ones, e.g. "<!--", "www", are well under this). */
#define GHOSTCON_LIGATURE_MAX_RUN_LEN 8

/* Scans `row` left-to-right for maximal runs of adjacent, same-style
   cells whose codepoint is in the ligature-trigger charset, shapes
   each one through the atlas's primary font, and -- only for runs
   where shaping preserved a 1:1 glyph-per-input-codepoint mapping
   (i.e. no reordering/merging; the common case for this trigger
   charset under a real font) -- writes one entry per cell whose
   shaped glyph differs from ghostcon_atlas_plain_glyph_index() for
   that cell's own codepoint. A run that shapes to a different glyph
   COUNT than its input (an actual multi-codepoint collapse, or
   HarfBuzz reordering) is left entirely unreported rather than
   guessed at -- out of scope for this first pass; those cells just
   render with their plain per-codepoint glyph, same as before this
   feature existed. Entries are written in ascending x order and never
   repeat an x. Returns the number of entries written (0 if `atlas`
   has no HarfBuzz font, i.e. harfbuzz wasn't available at build
   time). */
int ghostcon_ligature_scan_row(const ghostcon_row_t *row, ghostcon_atlas_t *atlas,
                                ghostcon_ligature_glyph_t *out_glyphs, int max_glyphs);
