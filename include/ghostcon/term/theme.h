#pragma once

#include "color.h"

/* ------------------------------------------------------------------ */
/* Named theme presets                                                 */
/*                                                                     */
/* A small, curated set (not an attempt at Ghostty's full catalog of   */
/* hundreds -- that's mostly data entry with little design value; add  */
/* more here later if a specific one is wanted). Each preset sets the  */
/* palette's 16 ANSI colors plus fg/bg/cursor defaults; the 216-color  */
/* cube and grayscale ramp (232-255) are left untouched -- those are   */
/* derived/computed, not part of any theme's identity, matching how    */
/* every terminal emulator's theme format works. Config layering (see  */
/* config.h's `theme`/`[colors]`) applies a preset first, then any     */
/* explicit per-color overrides on top -- same precedent as [cursor]'s */
/* preset-then-explicit-path layering.                                 */
/* ------------------------------------------------------------------ */

/* Applies the named preset to `pal` (16 ANSI colors + fg/bg/cursor).
   Returns false (leaving `pal` untouched) for an unrecognized name --
   not an error, same "unrecognized falls back to default" convention
   as antialiasing/etc. NULL/empty name always returns false. */
bool ghostcon_theme_apply(ghostcon_palette_t *pal, const char *name);
