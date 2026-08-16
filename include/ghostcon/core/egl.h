#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <gbm.h>

/* ------------------------------------------------------------------ */
/* GBM + EGL/GLES2 context                                             */
/*                                                                     */
/* Opens a DRM node and creates a GBM-backed EGL window surface plus a */
/* GLES2 context. GBM buffer allocation for *rendering* doesn't need   */
/* DRM master — only scanning a buffer out to a display does (that's   */
/* core/kms.c's job, layered on top of this). This means the same      */
/* setup here works both headless (render node, e.g. renderD128 — used */
/* by the render smoke test) and for the real display path (primary    */
/* node, e.g. card0/card1 — used once kms.c owns the DRM master).      */
/* ------------------------------------------------------------------ */

typedef struct {
    int drm_fd;
    struct gbm_device  *gbm_dev;
    struct gbm_surface *gbm_surf;
    bool owns_gbm; /* false when gbm_dev/gbm_surf are borrowed (see
                      ghostcon_egl_init_with_gbm) — deinit then leaves
                      them for the owner (core/kms.c) to destroy */
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    uint32_t width, height;
} ghostcon_egl_t;

/* Opens drm_path itself and creates its own render-only GBM
   device/surface — the headless path (render node, no DRM master). */
bool ghostcon_egl_init(ghostcon_egl_t *egl, const char *drm_path,
                        uint32_t width, uint32_t height);

/* Binds to an already-created GBM device/surface instead of making its
   own — the real display path: core/kms.c creates a *scanout*-capable
   surface (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) tied to the CRTC
   it resolved, and this just wraps that surface in an EGL context.
   gbm_dev/gbm_surf remain owned by the caller (kms.c); deinit does not
   destroy them. */
bool ghostcon_egl_init_with_gbm(ghostcon_egl_t *egl,
                                 struct gbm_device *gbm_dev,
                                 struct gbm_surface *gbm_surf,
                                 uint32_t width, uint32_t height);

void ghostcon_egl_deinit(ghostcon_egl_t *egl);

bool ghostcon_egl_make_current(ghostcon_egl_t *egl);
bool ghostcon_egl_swap(ghostcon_egl_t *egl);
