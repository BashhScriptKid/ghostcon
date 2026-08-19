#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "atlas.h"

/* Forward-declared rather than including term/kitty_graphics.h -- this
   header only ever needs a pointer to it (see
   ghostcon_gles_kitty_tex_get below), same reasoning as screen.h's own
   forward-declare of struct ghostcon_screen for selection.h. */
struct ghostcon_kitty_graphics;

/* ------------------------------------------------------------------ */
/* GLES 2.0 renderer                                                   */
/*                                                                     */
/* One shader program, one atlas texture. A quad is either a solid     */
/* fill (background cell, cursor block — sampled from a reserved       */
/* opaque texel at the atlas origin) or a glyph (sampled from the      */
/* glyph's UV rect, alpha = per-pixel coverage). Both go through the   */
/* same textured-quad path per PLAN.md's "Renderer design": draw       */
/* background quads first, then glyph quads on top with alpha          */
/* blending. See render/machine.c for turning screen damage into the   */
/* quad list this consumes.                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    float x, y;         /* screen-space pixel position, top-left origin */
    float u, v;
    float r, g, b, a;
    float bg_r, bg_g, bg_b; /* cell background, for gamma alpha correction --
                                equal to r/g/b on non-glyph quads, a no-op */
    float is_glyph; /* 1.0 for glyph quads (ghostcon_gles_push_glyph),
                        0.0 for everything else -- selects the shader's
                        per-channel subpixel-mix path vs. the plain
                        alpha-blended path. See FRAG_SRC's doc comment
                        in gles.c. */
} ghostcon_vertex_t;

typedef struct ghostcon_gles ghostcon_gles_t;

/* Must be called with an EGL context current (see core/egl.h).
   want_linear_blending: pass the current ghostcon_egl_t's own
   srgb_colorspace field -- this function independently checks for
   GL_EXT_sRGB_write_control (a current-context GL check egl.c can't
   do itself) before actually enabling GL_FRAMEBUFFER_SRGB_EXT, so
   both conditions are required, not just this one. See FRAG_SRC's
   doc comment in gles.c for what this actually changes (not what
   you'd guess). */
ghostcon_gles_t *ghostcon_gles_create(uint32_t viewport_w, uint32_t viewport_h,
                                       bool want_linear_blending);
void ghostcon_gles_destroy(ghostcon_gles_t *gles);

void ghostcon_gles_resize(ghostcon_gles_t *gles, uint32_t w, uint32_t h);

/* Uploads the atlas bitmap to the GPU texture if ghostcon_atlas_dirty()
   is true (and clears the dirty flag) — UNLESS `force` is set, which
   uploads unconditionally regardless of the dirty flag. Glyphs are
   rasterized lazily on first use (see render/machine.c calling
   ghostcon_atlas_glyph), so this must be called AFTER the frame's
   render_dirty pass, not before — syncing first uploads a stale/empty
   atlas and glyphs silently don't appear (no GL error, since the
   texture is valid, just outdated). Call order: begin -> render_dirty
   (pushes quads, rasterizes new glyphs into the CPU-side atlas) ->
   sync_atlas (uploads them) -> end (draws, using the now-current
   texture).
   `force=true` is required exactly once per `ghostcon_gles_t`
   instance, right after creating it (before its first end()) — the
   dirty flag tracks changes to the CPU-side atlas bitmap, which is a
   separate, longer-lived object than any one GLES instance's GPU
   texture. A brand-new instance's texture is created empty
   (glGenTextures, no data yet); if the atlas bitmap happens to already
   be "not dirty" at that point (nothing new rasterized since some
   earlier instance's last sync), a dirty-gated sync_atlas call would
   skip uploading anything, leaving that fresh texture "incomplete" per
   the GL spec (never given level-0 data) — sampling it is
   implementation-defined, and on at least one real driver this
   silently returned solid opaque alpha, making every glyph quad render
   as a filled rectangle instead of its actual shape. Found live, after
   a VT reacquire (which always creates a fresh ghostcon_gles_t) —
   see core/main.c's acquire_display(). */
void ghostcon_gles_sync_atlas(ghostcon_gles_t *gles, ghostcon_atlas_t *atlas, bool force);

/* Resets the quad batch for a new frame, clearing the framebuffer to
   (bg_r, bg_g, bg_b) first iff `clear` is true.
   Pass `clear=true` (paired with the caller redrawing the FULL
   accumulated dirty region, not just cells changed since the previous
   call) every frame, not just the first. A tempting-looking
   optimization — clear only once, then only redraw what's newly dirty,
   relying on render/machine.c's per-cell background quads to repaint
   just the changed rows — is WRONG here: core/kms.c's GBM/EGL surface
   rotates across multiple physical buffers for tear-free presentation,
   and each buffer has its own independent history of what's been
   painted onto it. "Only redraw what changed since the last render()
   call" implicitly assumes a single continuous buffer; against N
   rotating buffers, each one only receives the damage from every Nth
   frame, so already-rendered content silently disappears and
   reappears on alternating frames. Found live: characters flashed in
   and out on literally every keystroke, because only the cursor's row
   was (correctly) marked dirty that frame, but only every-other
   physical buffer had actually received that specific row's repaint.
   Proper per-buffer damage tracking would make the incremental version
   correct, but is real complexity this doesn't need yet — see
   core/main.c's render_frame() and PLAN.md's own renderer design note
   ("redrawing all dirty cells each frame is sufficient" for a
   terminal at this scale, which in practice means every frame, full
   stop, not just first-frame-after-acquire). */
void ghostcon_gles_begin(ghostcon_gles_t *gles, bool clear,
                          float bg_r, float bg_g, float bg_b);

/* Appends an opaque solid-color rect (background cell / cursor block). */
void ghostcon_gles_push_rect(ghostcon_gles_t *gles,
                              float x, float y, float w, float h,
                              float r, float g, float b, float a);

/* Appends a glyph quad at (x, y) sized (w, h), sampling `glyph`'s UV
   rect, tinted with the foreground color. bg_r/g/b is the cell's
   resolved background color, used only for the gamma alpha
   correction curve (see ghostcon_gles_set_gamma_correct below). */
void ghostcon_gles_push_glyph(ghostcon_gles_t *gles,
                               float x, float y, float w, float h,
                               const ghostcon_glyph_t *glyph,
                               float r, float g, float b, float a,
                               float bg_r, float bg_g, float bg_b);

/* Whether glyph edges get the luminance-based alpha correction curve
   (ported from Ghostty's cell_text.f.glsl) that biases FreeType's raw
   coverage value to look crisper against the cell's actual background,
   instead of a naive straight alpha blend. Defaults to on; takes
   effect on the next ghostcon_gles_end(). */
void ghostcon_gles_set_gamma_correct(ghostcon_gles_t *gles, bool enabled);

/* True if ghostcon_gles_create()'s want_linear_blending actually
   resulted in a working offscreen sRGB FBO + blit pipeline (both the
   needed extensions present AND the FBO completed) -- decided once at
   creation time, not something that changes later. False doesn't mean
   anything is broken; it just means rendering fell back to the plain
   path, silently, same as an unrecognized antialiasing value falling
   back to grayscale. Purely informational (e.g. startup logging) --
   nothing else needs to branch on this, the shader already does. */
bool ghostcon_gles_linear_blending_active(const ghostcon_gles_t *gles);

/* Issues the draw calls for everything pushed since ghostcon_gles_begin,
   then eglSwapBuffers via the caller's ghostcon_egl_t. */
void ghostcon_gles_end(ghostcon_gles_t *gles);

/* ------------------------------------------------------------------ */
/* Arbitrary-texture images (Kitty graphics protocol placements)       */
/*                                                                     */
/* Deliberately a separate pipeline from the atlas-based batch above:  */
/* each Kitty image is its own arbitrary-sized GPU texture, not        */
/* something the shared glyph atlas allocator manages, and GLES2 can't */
/* sample a different texture per vertex within one draw call. Each    */
/* image placement is its own immediate glDrawArrays call instead of   */
/* being folded into the push_rect/push_glyph batch. See render/       */
/* machine.c for how z<0 vs z>=0 placements get ordered relative to    */
/* that batch (call timing IS paint order here, not vertex order).     */
/* ------------------------------------------------------------------ */

typedef struct ghostcon_gles_image ghostcon_gles_image_t;

/* Uploads `rgba` (width*height*bpp bytes, bpp 3=RGB or 4=RGBA, no row
   padding) as a new GPU texture. Returns NULL on failure. */
ghostcon_gles_image_t *ghostcon_gles_image_create(const uint8_t *pixels,
                                                   int width, int height, int bpp);
void ghostcon_gles_image_destroy(ghostcon_gles_image_t *img);

/* Draws one placement immediately (its own draw call, executes right
   now, not deferred) -- use for z<0 placements, called before the
   frame's render_dirty/push_glyph batch so text ends up on top.
   src_w/src_h == 0 means "full image" (src_x/src_y ignored). alpha
   scales the whole placement's opacity (1.0 = fully opaque). Must be
   called between ghostcon_gles_begin and ghostcon_gles_end. */
void ghostcon_gles_draw_image_now(ghostcon_gles_t *gles, ghostcon_gles_image_t *img,
                                  float x, float y, float w, float h,
                                  float src_x, float src_y, float src_w, float src_h,
                                  float alpha);

/* Defers a placement draw until after ghostcon_gles_end()'s main batch
   draw call (but still before the frame's swap/blit) -- use for z>=0
   placements, so they paint on top of text. Same parameters as
   ghostcon_gles_draw_image_now. The queue is drained and cleared by
   the next ghostcon_gles_end() call; nothing needs to be called to
   reset it between frames. */
void ghostcon_gles_queue_image(ghostcon_gles_t *gles, ghostcon_gles_image_t *img,
                               float x, float y, float w, float h,
                               float src_x, float src_y, float src_w, float src_h,
                               float alpha);

/* Returns a GPU texture for Kitty image `image_id`, uploading from
   `pixels` on first use and re-uploading whenever `generation` differs
   from what's currently cached (the term-layer image store bumps its
   generation counter each time an id is re-transmitted -- see
   term/kitty_graphics.h). The returned handle is owned by `gles`:
   never call ghostcon_gles_image_destroy on it, and don't hold onto
   it past the current frame (a later re-transmission can invalidate
   and replace it). Bounded to the same max-image count as the
   term-layer store; under pressure, evicts whichever cached entry's
   id doesn't currently resolve via ghostcon_kitty_graphics_find_image
   first (this needs the same screen's kitty_graphics to check that),
   falling back to simple oldest-first if every cached entry is still
   live. Returns NULL on allocation failure. */
ghostcon_gles_image_t *ghostcon_gles_kitty_tex_get(ghostcon_gles_t *gles,
                                                   const struct ghostcon_kitty_graphics *kg,
                                                   uint32_t image_id, uint32_t generation,
                                                   const uint8_t *pixels, int width, int height, int bpp);

/* Reads back the just-rendered frame and writes it out as a binary PPM
   (P6). MUST be called after ghostcon_gles_end() but before the
   caller's ghostcon_egl_swap() -- glReadPixels reads whatever is
   currently bound as the draw framebuffer, which is only guaranteed
   to hold this frame's actual content in that window; past a swap,
   an EGL surface's backbuffer is free to come back with undefined
   content (multi-buffered presentation, no EGL_SWAP_BEHAVIOR_PRESERVED
   guarantee here). Ground-truth pixel capture for comparing against a
   reference render when a visual difference is hard to describe in
   words -- see core/main.c's Ctrl+Alt+D dump hotkey. */
bool ghostcon_gles_screenshot_ppm(ghostcon_gles_t *gles, const char *path);
