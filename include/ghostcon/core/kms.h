#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* ------------------------------------------------------------------ */
/* DRM/KMS mode setting + atomic page flip                             */
/*                                                                     */
/* Live-tested on the real machine as of this writing (PLAN.md has     */
/* many rounds of live verification: login/rendering fixes, the        */
/* leftover-cursor-plane fix, the hardware mouse cursor below) — the   */
/* "untested at runtime" note that used to be here is stale. Follows   */
/* the standard atomic-KMS pattern (query connector/CRTC/plane         */
/* properties once at init, then commit only FB_ID/CRTC_ID + geometry  */
/* deltas per frame) — see e.g. the kernel's drm-howto or Weston's      */
/* drm-backend for the same shape of code.                             */
/*                                                                     */
/* Ownership split: this module does NOT call drmSetMaster/            */
/* drmDropMaster itself — that's tied to the VT_PROCESS acquire/       */
/* release lifecycle, which core/vtctl.c owns (PLAN.md "vtctl.c").     */
/* Callers must already hold DRM master on `drm_path`'s fd before      */
/* calling ghostcon_kms_init.                                          */
/* ------------------------------------------------------------------ */

/* Interaction states the hardware cursor sprite can be in -- each has
   its own independently-loaded image (so switching between them, e.g.
   on hovering an OSC-8 hyperlink cell, is a cheap FB swap rather than
   a decode-and-reupload). See PLAN.md's "Cursor sprite: raster images,
   per-state, config-driven" section for the full design. */
typedef enum {
    GC_CURSOR_STATE_DEFAULT,
    GC_CURSOR_STATE_LINK,
    GC_CURSOR_STATE_COUNT,
} ghostcon_cursor_state_t;

typedef struct {
    int fd; /* not owned — caller (core/vtctl.c) manages master/VT lifecycle */

    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    drmModeModeInfo mode;
    uint32_t width, height;
    uint32_t mode_blob_id;

    /* Cached atomic property IDs, resolved once at init by name so we
       never re-walk drmModeObjectGetProperties() per frame. */
    struct {
        uint32_t crtc_id, fb_id, src_x, src_y, src_w, src_h,
                 crtc_x, crtc_y, crtc_w, crtc_h;
    } plane_props;
    struct {
        uint32_t mode_id, active;
    } crtc_props;
    struct {
        uint32_t crtc_id;
    } connector_props;

    /* Hardware mouse cursor -- a real DRM CURSOR-type plane, not a
       GLES quad drawn into the primary framebuffer, so cursor movement
       latency is independent of content rendering (see
       ghostcon_kms_move_cursor()'s own doc comment). cursor_plane_id
       is 0 if no CURSOR-type plane was found for this CRTC -- not
       fatal, the cursor just never renders, matching this tree's usual
       tolerance for optional hardware features. disable_other_planes()
       (kms.c) deliberately excludes this plane alongside our own
       primary plane, so it doesn't turn off the very cursor plane
       we're now using ourselves. */
    uint32_t cursor_plane_id;
    struct {
        uint32_t crtc_id, fb_id, src_x, src_y, src_w, src_h,
                 crtc_x, crtc_y, crtc_w, crtc_h;
    } cursor_plane_props;
    uint32_t cursor_w, cursor_h;     /* fixed square canvas size, same for every state
                                         (see draw_cursor_ibeam's own doc comment on why
                                         square -- an amdgpu cursor-plane constraint) */
    uint32_t cursor_fb_id[GC_CURSOR_STATE_COUNT];
    uint32_t cursor_bo_handle[GC_CURSOR_STATE_COUNT]; /* dumb buffers backing cursor_fb_id[], for cleanup */
    uint32_t cursor_hot_x[GC_CURSOR_STATE_COUNT], cursor_hot_y[GC_CURSOR_STATE_COUNT];
    ghostcon_cursor_state_t cursor_active_state; /* which state's FB is currently bound, if cursor_enabled */
    bool     cursor_enabled;         /* has the plane been committed visible yet? */
    int      cursor_last_x, cursor_last_y; /* last position passed to ghostcon_kms_move_cursor(),
                                               so ghostcon_kms_set_cursor_state() can re-commit at
                                               the same spot without the caller re-supplying it */

    /* Scanout-capable GBM surface — pass gbm_dev/gbm_surf here into
       core/egl.h's ghostcon_egl_init-equivalent setup instead of a
       render-only surface (see ghostcon_kms_create_scanout_surface). */
    struct gbm_device  *gbm_dev;
    struct gbm_surface *gbm_surf;

    struct gbm_bo *current_bo;   /* currently scanned out */
    uint32_t       current_fb_id;
    bool           flip_pending;
} ghostcon_kms_t;

/* drm_fd must already be open with DRM master held (core/vtctl.c's job
   — see the ownership note above). Not closed by ghostcon_kms_deinit. */
bool ghostcon_kms_init(ghostcon_kms_t *kms, int drm_fd);
void ghostcon_kms_deinit(ghostcon_kms_t *kms);

/* Creates kms->gbm_surf at the chosen mode's resolution
   (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING). Call before creating the
   EGL surface that wraps it. */
bool ghostcon_kms_create_scanout_surface(ghostcon_kms_t *kms);

/* Blocking atomic commit that brings the CRTC/connector online showing
   whatever was just rendered into the GBM surface (call once, after
   the first eglSwapBuffers). */
bool ghostcon_kms_modeset(ghostcon_kms_t *kms);

/* Non-blocking atomic page flip to the most recently swapped GBM
   buffer (call after every subsequent eglSwapBuffers). Waits for the
   previous flip's completion event first if one is still in flight —
   single-buffered flip queue, matching PLAN.md's "no zero-copy"
   simplicity note for the renderer. */
bool ghostcon_kms_page_flip(ghostcon_kms_t *kms);

/* Moves the hardware cursor sprite to (x, y) in screen pixel
   coordinates (top-left origin, same space as everything else in this
   tree) -- the ACTIVE state's own hot_x/hot_y (see
   ghostcon_kms_set_cursor_state()) is subtracted internally, so callers
   always pass the raw pointer position, never a pre-adjusted one. A
   cheap atomic commit touching only CRTC_X/CRTC_Y (plus CRTC_ID/FB_ID
   on the very first call, to enable the plane) -- deliberately NOT
   part of ghostcon_kms_page_flip()'s per-content-frame path, so cursor
   movement latency doesn't depend on how often the terminal content
   itself needs to redraw. No-op (returns true) if no CURSOR-type plane
   was found at init (kms->cursor_plane_id == 0) or the active state's
   image failed to set up -- matches this module's usual "optional
   hardware feature" tolerance. */
bool ghostcon_kms_move_cursor(ghostcon_kms_t *kms, int x, int y);

/* Sets/replaces the cursor image for one state (GC_CURSOR_STATE_DEFAULT/
   LINK). `pixels` (ARGB8888, row-major, w*h) is letterboxed (centered,
   transparent padding) into the fixed square canvas -- same amdgpu
   square-cursor-plane constraint the procedural path already handles.
   `pixels == NULL` means "no raster asset for this state" -- falls
   back to the procedural I-beam (kmscon's own generate_ibeam_cursor()
   algorithm, src/terminal.c -- a white stem with top/bottom serifs,
   dark halo outline) sized from `fallback_font_height` (typically the
   terminal's current cell_h), preserving the original behavior when
   nothing is configured. Safe to call repeatedly (e.g. on a config
   hot-reload, or a font_size zoom changing fallback_font_height) --
   destroys any previous image/FB for this state first. No-op (returns
   true) if no CURSOR-type plane was found. If this state is the
   currently-active one and the plane is already visible, it's
   re-enabled on the next ghostcon_kms_move_cursor() call so the new
   image actually gets committed (FB_ID isn't touched by
   move_cursor()'s fast path once already enabled). */
bool ghostcon_kms_set_cursor_image(ghostcon_kms_t *kms, ghostcon_cursor_state_t state,
                                    const uint32_t *pixels, uint32_t w, uint32_t h,
                                    uint32_t hot_x, uint32_t hot_y,
                                    unsigned int fallback_font_height);

/* Switches which pre-loaded state's image is bound to the plane --
   e.g. call when the pointer moves onto/off an OSC-8 hyperlink cell.
   A cheap atomic commit (same class as ghostcon_kms_move_cursor()).
   No-op if `state` is already the active one (avoids a redundant
   commit on every motion event while hovering the same state
   continuously). */
bool ghostcon_kms_set_cursor_state(ghostcon_kms_t *kms, ghostcon_cursor_state_t state);
