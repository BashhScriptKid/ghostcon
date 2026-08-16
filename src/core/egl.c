#define _DEFAULT_SOURCE /* O_CLOEXEC under -std=c11 */

#include "ghostcon/core/egl.h"

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Shared by ghostcon_egl_init (owns gbm_dev, creates its own render-only
   gbm_surf) and ghostcon_egl_init_with_gbm (borrows both from the
   caller, e.g. core/kms.c's scanout-capable surface). `borrowed_surf`
   NULL means "create our own"; non-NULL means "wrap this one". */
static bool
init_common(ghostcon_egl_t *egl, struct gbm_surface *borrowed_surf,
            uint32_t width, uint32_t height)
{
    egl->width = width;
    egl->height = height;

    egl->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, egl->gbm_dev, NULL);
    if (egl->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "egl: eglGetPlatformDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl->display, &major, &minor)) {
        fprintf(stderr, "egl: eglInitialize failed: 0x%x\n", eglGetError());
        goto fail_display;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "egl: eglBindAPI failed\n");
        goto fail_display;
    }

    static const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl->display, config_attrs, &config, 1, &num_configs) ||
        num_configs < 1) {
        fprintf(stderr, "egl: eglChooseConfig failed\n");
        goto fail_display;
    }

    if (borrowed_surf) {
        egl->gbm_surf = borrowed_surf;
    } else {
        EGLint visual_id;
        if (!eglGetConfigAttrib(egl->display, config, EGL_NATIVE_VISUAL_ID, &visual_id)) {
            fprintf(stderr, "egl: eglGetConfigAttrib(NATIVE_VISUAL_ID) failed\n");
            goto fail_display;
        }
        egl->gbm_surf = gbm_surface_create(egl->gbm_dev, width, height,
                                            (uint32_t)visual_id,
                                            GBM_BO_USE_RENDERING);
        if (!egl->gbm_surf) {
            fprintf(stderr, "egl: gbm_surface_create failed\n");
            goto fail_display;
        }
    }

    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl->context = eglCreateContext(egl->display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (egl->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "egl: eglCreateContext failed: 0x%x\n", eglGetError());
        goto fail_gbm_surf;
    }

    egl->surface = eglCreateWindowSurface(egl->display, config,
                                           (EGLNativeWindowType)egl->gbm_surf, NULL);
    if (egl->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "egl: eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        goto fail_context;
    }

    return true;

fail_context:
    eglDestroyContext(egl->display, egl->context);
    egl->context = EGL_NO_CONTEXT;
fail_gbm_surf:
    if (!borrowed_surf && egl->gbm_surf) {
        gbm_surface_destroy(egl->gbm_surf);
        egl->gbm_surf = NULL;
    }
fail_display:
    eglTerminate(egl->display);
    egl->display = EGL_NO_DISPLAY;
    return false;
}

bool
ghostcon_egl_init(ghostcon_egl_t *egl, const char *drm_path,
                   uint32_t width, uint32_t height)
{
    memset(egl, 0, sizeof(*egl));
    egl->owns_gbm = true;

    egl->drm_fd = open(drm_path, O_RDWR | O_CLOEXEC);
    if (egl->drm_fd < 0) {
        fprintf(stderr, "egl: open %s: %s\n", drm_path, strerror(errno));
        return false;
    }

    egl->gbm_dev = gbm_create_device(egl->drm_fd);
    if (!egl->gbm_dev) {
        fprintf(stderr, "egl: gbm_create_device failed\n");
        close(egl->drm_fd);
        return false;
    }

    if (!init_common(egl, NULL, width, height)) {
        gbm_device_destroy(egl->gbm_dev);
        close(egl->drm_fd);
        memset(egl, 0, sizeof(*egl));
        return false;
    }
    return true;
}

bool
ghostcon_egl_init_with_gbm(ghostcon_egl_t *egl, struct gbm_device *gbm_dev,
                            struct gbm_surface *gbm_surf,
                            uint32_t width, uint32_t height)
{
    memset(egl, 0, sizeof(*egl));
    egl->drm_fd = -1;
    egl->owns_gbm = false;
    egl->gbm_dev = gbm_dev;

    if (!init_common(egl, gbm_surf, width, height)) {
        memset(egl, 0, sizeof(*egl));
        return false;
    }
    return true;
}

void
ghostcon_egl_deinit(ghostcon_egl_t *egl)
{
    if (egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->surface != EGL_NO_SURFACE)
            eglDestroySurface(egl->display, egl->surface);
        if (egl->context != EGL_NO_CONTEXT)
            eglDestroyContext(egl->display, egl->context);
        eglTerminate(egl->display);
    }
    if (egl->owns_gbm) {
        if (egl->gbm_surf)
            gbm_surface_destroy(egl->gbm_surf);
        if (egl->gbm_dev)
            gbm_device_destroy(egl->gbm_dev);
    }
    if (egl->drm_fd >= 0)
        close(egl->drm_fd);
    memset(egl, 0, sizeof(*egl));
}

bool
ghostcon_egl_make_current(ghostcon_egl_t *egl)
{
    return eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);
}

bool
ghostcon_egl_swap(ghostcon_egl_t *egl)
{
    return eglSwapBuffers(egl->display, egl->surface);
}
