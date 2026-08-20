#ifndef GHOSTCON_TERM_SIXEL_H
#define GHOSTCON_TERM_SIXEL_H

/* Sixel graphics (DCS <P1>;<P2>;<P3> q <sixel-data> ST) -- decode and
   placement of DEC sixel raster images.

   Architecturally simpler than Kitty graphics (term/kitty_graphics.h):
   sixel has no ids, no separate transmit/display split, no z-order,
   no cropping or cell-scaling -- a sixel sequence IS an immediate
   "decode this raster and stamp it at the cursor" command, closer to
   printing a very tall multi-column character than to Kitty's
   image-object model. So instead of images[]+placements[] keyed by
   client-chosen id, this is just a flat array of already-decoded,
   already-placed images, each remembering where it was stamped.

   Same "renderer reads a side table, doesn't own compositing" shape
   as kitty_graphics.h (see that header's own doc comment, and
   CLAUDE.md's note on why erase-only backgrounds are a content tag
   rather than a cell rewrite -- the same reasoning applies here):
   this module never touches screen state directly, and the renderer
   (render/machine.c) walks ghostcon_screen_active_sixel_state()'s
   placements[] itself. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GHOSTCON_SIXEL_MAX_PLACEMENTS   64
#define GHOSTCON_SIXEL_MAX_DIM          4096u
#define GHOSTCON_SIXEL_MAX_IMAGE_BYTES  (16u * 1024u * 1024u) /* decoded RGBA cap, per image */
#define GHOSTCON_SIXEL_MAX_TOTAL_BYTES  (128u * 1024u * 1024u) /* across all placements */
#define GHOSTCON_SIXEL_COLOR_REGISTERS  256

typedef struct {
    bool     in_use;
    int32_t  width, height;
    uint8_t *pixels; /* owned, width*height*4 bytes, RGBA */
    size_t   pixel_len;
    int32_t  anchor_col, anchor_row; /* cursor cell position at placement time */
    /* Bumped every time this slot is (re)populated -- same GPU
       texture-cache-invalidation purpose as
       ghostcon_kitty_image_t.generation (see that struct's own doc
       comment). */
    uint32_t generation;
} ghostcon_sixel_placement_t;

typedef struct ghostcon_sixel_state {
    ghostcon_sixel_placement_t placements[GHOSTCON_SIXEL_MAX_PLACEMENTS];
    size_t                     total_bytes;
} ghostcon_sixel_state_t;

void ghostcon_sixel_state_init(ghostcon_sixel_state_t *st);
void ghostcon_sixel_state_deinit(ghostcon_sixel_state_t *st);

/* Same shape as ghostcon_kitty_cursor_move_t (kitty_graphics.h) --
   see that struct's own doc comment. Sixel has no C=1-equivalent
   suppression key, so moved is unconditionally true whenever a valid
   image was actually decoded and placed. */
typedef struct {
    bool    moved;
    int32_t rows;
    int32_t col;
} ghostcon_sixel_cursor_move_t;

/* Decodes one complete DCS sixel command and stamps the result as a
   new placement at (cursor_col, cursor_row). dcs_params/params_count
   are the DCS-introducer parameters (P1 aspect ratio -- parsed but
   not acted on, no real terminal's rendering depends on it; P2
   background-select; P3 unused/obsolete grid size, ignored), exactly
   as accumulated by term/stream.c's DCS state machine. body/body_len
   is the sixel data itself (raster-attribute command, color-register
   definitions, and the sixel character stream), NOT including the
   DCS introducer or the ST terminator. cell_w/cell_h are the
   renderer's current cell pixel size, needed for the post-placement
   cursor move -- pass 0 for either to suppress it. */
void ghostcon_sixel_decode(ghostcon_sixel_state_t *st,
                           const int32_t *dcs_params, size_t params_count,
                           const char *body, size_t body_len,
                           int32_t cursor_col, int32_t cursor_row,
                           int32_t cell_w, int32_t cell_h,
                           ghostcon_sixel_cursor_move_t *out_move);

#endif
