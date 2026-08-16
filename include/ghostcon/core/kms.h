#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* ------------------------------------------------------------------ */
/* DRM/KMS mode setting + atomic page flip                             */
/*                                                                     */
/* UNTESTED AT RUNTIME as of this writing — see PLAN.md's Phase 1 item */
/* 2 note. This machine has one GPU whose primary node is already DRM  */
/* master-held by the live desktop session; exercising this module     */
/* means switching to a free VT (visibly interrupting that session),   */
/* deferred until done together with the user. Compiles clean and      */
/* follows the standard atomic-KMS pattern (query connector/CRTC/plane */
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
