#pragma once

#include "ghostcon/term/screen.h"
#include "atlas.h"
#include "gles.h"

/* ------------------------------------------------------------------ */
/* Damage -> render command generation                                 */
/*                                                                     */
/* Walks the screen's dirty region (ghostcon_screen_get_dirty) and     */
/* pushes background + glyph quads for every cell in every dirty row   */
/* into `gles`. Does not call ghostcon_gles_begin/end — the caller     */
/* frames the batch (see IMPLEMENTATION.md "Renderer Architecture"     */
/* pipeline: begin -> render_dirty -> end -> swap).                    */
/* ------------------------------------------------------------------ */

void ghostcon_machine_render_dirty(ghostcon_screen_t *screen,
                                    ghostcon_atlas_t *atlas,
                                    ghostcon_gles_t *gles,
                                    int cell_w, int cell_h);

/* Pushes a solid-color quad for the cursor at its current screen
   position, shaped per screen->cursor.cursor_style (block/underline/
   bar), unless DECTCEM (screen->cursor_visible) is off. No blink
   timer -- the _BLINK style variants render identically to their
   steady counterparts for now. Called every frame alongside
   render_dirty (same "just redraw everything" approach the rest of
   this renderer already uses), not just on cursor-moved frames. */
void ghostcon_machine_render_cursor(ghostcon_screen_t *screen,
                                     ghostcon_gles_t *gles,
                                     int cell_w, int cell_h);
