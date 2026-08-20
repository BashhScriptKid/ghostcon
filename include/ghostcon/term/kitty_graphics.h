#ifndef GHOSTCON_TERM_KITTY_GRAPHICS_H
#define GHOSTCON_TERM_KITTY_GRAPHICS_H

/* Kitty graphics protocol (APC _G...ST) -- transmission, placement, and
   deletion of images. All four transmission mediums are supported:

   - t=d (direct): image bytes embedded in the escape sequence itself.
   - t=s (shared-memory): via POSIX shm_open -- see read_shm_segment()
     in kitty_graphics.c. Read-only, never O_CREAT (can only read a
     segment the client already created), and always shm_unlink()s it
     afterward, success or failure.
   - t=t (temporary-file) and t=f (plain file): both go through
     read_local_file() in kitty_graphics.c, which opens the client-
     supplied path FIRST and only resolves/validates its real path
     (via /proc/self/fd, reflecting the actual open inode) AFTER --
     closing the TOCTOU/symlink-swap gap a naive validate-then-open
     would leave, matching Ghostty's own readFile()/
     validatedFilePath() (itself a port of Kitty's reference
     terminal). Both are restricted to /tmp or /dev/shm; NOT the
     genuinely-arbitrary-path t=f the Kitty spec itself defines (real
     Kitty/Ghostty instead gate unrestricted t=f behind an explicit
     opt-in config flag, off by default -- ghostcon has no such flag,
     so t=f stays bounded the same way t=t is rather than being wide
     open with nothing gating it). They differ in two ways: t=t
     additionally requires the filename to contain Kitty's own
     "tty-graphics-protocol" naming convention and always deletes the
     file afterward (ownership of a *temporary* file's cleanup passes
     to the terminal, same posture as t=s's shm_unlink()); t=f skips
     the filename check and never deletes anything, since it's treated
     as a read-only cache the client expects to still exist afterward
     (e.g. re-displaying the same pre-rendered image again later)
     rather than a one-shot handoff.

   Raw pixel formats (f=24 RGB, f=32 RGBA) and PNG (f=100, decoded via
   libpng) are both supported regardless of medium.

   This module is intentionally decoupled from ghostcon_screen_t: it
   receives the cursor position as plain ints from the caller rather
   than reaching into screen state itself, and reports placements back
   through an accessor rather than writing into the cell grid -- image
   compositing is a renderer-side concern, not a screen-model one (see
   CLAUDE.md's note on why erase-only backgrounds are a content tag,
   not a cell rewrite; the same "renderer reads a side table" shape
   applies here). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Same signature as ghostcon_output_fn (term/stream.h) -- duplicated
   rather than included to avoid a stream.h <-> kitty_graphics.h
   circular include; the two are structurally compatible function
   pointer types so no cast is needed at the call site. */
typedef void (*ghostcon_kitty_output_fn)(void *userdata,
                                         const uint8_t *data, size_t len);

#define GHOSTCON_KITTY_MAX_IMAGES      64
#define GHOSTCON_KITTY_MAX_PLACEMENTS  256
#define GHOSTCON_KITTY_MAX_DIM         4096u
#define GHOSTCON_KITTY_MAX_IMAGE_BYTES (16u * 1024u * 1024u)  /* per-image decoded cap */
#define GHOSTCON_KITTY_MAX_TOTAL_BYTES (128u * 1024u * 1024u) /* across all stored images */

typedef struct {
    bool     in_use;
    uint32_t id;
    int32_t  width, height;
    int32_t  bpp; /* 3 (RGB) or 4 (RGBA) */
    uint8_t *pixels; /* owned, width*height*bpp bytes */
    size_t   pixel_len;
    /* Bumped every time this slot's pixels are (re)populated by a
       transmission. Lets the renderer, which caches one GPU texture
       per image id, tell "same id, same data, texture still valid"
       apart from "same id, re-transmitted, texture is stale" without
       the term/ layer knowing anything about textures itself. */
    uint32_t generation;

    /* Cross-chunk (m=1) transmission in progress for this id. */
    bool     receiving;
    uint8_t *recv_buf; /* owned, capacity == expected pixel_len */
    size_t   recv_len;
} ghostcon_kitty_image_t;

typedef struct {
    bool     in_use;
    uint32_t image_id;
    uint32_t placement_id; /* 0 = anonymous */
    int32_t  anchor_col, anchor_row; /* cursor cell position at placement time */
    int32_t  z;
    /* Crop rect in source pixels; all zero means "full image". */
    int32_t  crop_x, crop_y, crop_w, crop_h;
    /* Placement size in cells; both zero means "natural pixel size". */
    int32_t  cell_cols, cell_rows;
} ghostcon_kitty_placement_t;

/* Tagged (not anonymous) so headers that only need a pointer (e.g.
   render/gles.h's ghostcon_gles_kitty_tex_get) can forward-declare
   `struct ghostcon_kitty_graphics;` without including this header. */
typedef struct ghostcon_kitty_graphics {
    ghostcon_kitty_image_t     images[GHOSTCON_KITTY_MAX_IMAGES];
    ghostcon_kitty_placement_t placements[GHOSTCON_KITTY_MAX_PLACEMENTS];
    size_t                     total_bytes;

    /* A multi-chunk (m=1) transmission's continuation chunks legally
       omit every key but m= (only the first chunk carries i=/a=/z=/
       crop/cell-scale/C=/p=, per spec) -- tracked here so the FINAL
       chunk, where the placement actually gets created, can resolve
       back to what the first chunk actually asked for instead of
       silently seeing "absent" (defaults) for all of it. -1 for the
       int64/int32 fields means no transfer in progress / value not
       yet captured.

       Found live via a real chunked transfer (a 48x48 PNG's base64
       exceeds the 4096-byte per-chunk cap): only image_id and the
       display flag were carried across chunks before this, so a
       chunked a=T,C=1 placement lost C=1 on the final chunk and
       ghostcon's own cursor auto-advance fired in ADDITION to
       whatever manual clearance the client was already doing itself
       (since C=1 exists specifically to prevent that) -- silently
       doubling the vertical gap after any image large enough to need
       chunking. z/crop/cell-scale have the exact same "only chunk 1
       carries it" exposure; fixed for all of them together rather
       than patching just the one symptom that happened to be
       reproduced first. */
    int64_t active_transfer_id;
    bool    active_transfer_display;
    int32_t active_transfer_z;
    int32_t active_transfer_crop_x, active_transfer_crop_y;
    int32_t active_transfer_crop_w, active_transfer_crop_h;
    int32_t active_transfer_cell_cols, active_transfer_cell_rows;
    int32_t active_transfer_no_cursor_move;
    int64_t active_transfer_placement_id;
} ghostcon_kitty_graphics_t;

void ghostcon_kitty_graphics_init(ghostcon_kitty_graphics_t *kg);
void ghostcon_kitty_graphics_deinit(ghostcon_kitty_graphics_t *kg);

/* Looks up an image by id (NULL if not present or not yet fully
   transmitted). Read-only accessor for the renderer -- see
   render/machine.c, which walks kg->placements[] itself (a plain
   fixed array, no accessor needed for that part) but needs this to
   resolve each placement's image_id back to pixel data. */
const ghostcon_kitty_image_t *ghostcon_kitty_graphics_find_image(
    const ghostcon_kitty_graphics_t *kg, uint32_t id);

/* Describes a cursor move the CALLER (term/stream.c, which owns the
   screen and its scroll-region-aware linefeed) should perform after a
   placement, matching Ghostty's own default (cursor_movement=.after,
   suppressed only by an explicit C=1 key): advance `rows` lines via
   normal linefeed semantics (so it scrolls correctly at the bottom of
   a scroll region, unlike just incrementing cursor.y), then set the
   column to `col`. This module never touches screen state directly --
   see this header's own top comment -- so it hands back what to do
   rather than doing it, keeping stream.c as the one place that
   actually knows how to move a cursor. moved=false means don't move
   it at all (C=1 was set, or the placement wasn't a screen-anchored
   one). */
typedef struct {
    bool    moved;
    int32_t rows;
    int32_t col;
} ghostcon_kitty_cursor_move_t;

/* Handle one complete, already ST-terminated APC "G..." body (the bytes
   after the 'G', up to but not including the terminator). cursor_col/
   cursor_row anchor any placement this command creates. cell_w/cell_h
   are the renderer's current cell pixel size, needed to compute how
   many rows/columns a placement occupies for the post-placement cursor
   move below -- pass 0 for either if unknown, which suppresses that
   move entirely (better than guessing wrong). Acks (OK/error) are
   written via output_fn per the command's q= quiet level. On return,
   *out_move (if non-NULL) describes any cursor move the caller should
   apply. */
void ghostcon_kitty_graphics_handle(ghostcon_kitty_graphics_t *kg,
                                    const char *body, size_t body_len,
                                    int32_t cursor_col, int32_t cursor_row,
                                    int32_t cell_w, int32_t cell_h,
                                    ghostcon_kitty_output_fn output_fn,
                                    void *output_userdata,
                                    ghostcon_kitty_cursor_move_t *out_move);

#endif
