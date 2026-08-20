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

/* Pushes one semi-transparent, alpha-blended rect per selected row
   (not per cell -- a char-kind selection's row range is always
   contiguous, see ghostcon_selection_row_range()) for the active text
   selection, if any. No-op if there's no active selection, or while
   scrolled back into history (screen->view_offset > 0) -- same
   reasoning ghostcon_machine_render_cursor() already uses: the grid
   coordinates would be pointing at spliced-in history content, not
   the live rows the selection actually belongs to. Called every frame
   alongside render_dirty/render_cursor -- no dirty-region tracking
   needed, ghostcon_gles_begin()'s full framebuffer clear each frame
   already means a cleared selection just stops being redrawn. */
void ghostcon_machine_render_selection(ghostcon_screen_t *screen,
                                        ghostcon_gles_t *gles,
                                        int cell_w, int cell_h);

/* Walks ghostcon_screen_active_kitty_graphics(screen)->placements[]
   (whichever screen -- primary or alt -- is currently active) and
   draws each one.
   z<0 placements are drawn immediately (their own draw call, right
   now) so they end up behind the text/background batch this frame's
   render_dirty will push; z>=0 placements are queued (see
   ghostcon_gles_queue_image's doc comment) so they draw after that
   batch, on top of text. Call this ONCE per frame, right after
   ghostcon_gles_begin and BEFORE render_dirty -- the z<0 half relies
   on executing before render_dirty's content is drawn (call order is
   paint order for the immediate half), while the z>=0 half just needs
   to be queued sometime before ghostcon_gles_end. No-op while
   scrolled back into history (screen->view_offset > 0), same
   reasoning as render_cursor/render_selection: placements are anchored
   to live cursor coordinates, not history-relative ones.

   Also walks ghostcon_screen_active_sixel_state(screen)->placements[]
   and queues each -- sixel has no z-order concept, so every sixel
   placement always draws above text (the same bucket Kitty's default
   z>=0 placements use). */
void ghostcon_machine_render_images(ghostcon_screen_t *screen,
                                     ghostcon_gles_t *gles,
                                     int cell_w, int cell_h);
