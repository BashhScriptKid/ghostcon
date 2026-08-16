/* See include/ghostcon/core/kms.h for the "untested at runtime" note. */

#include "ghostcon/core/kms.h"

#include <stdio.h>
#include <string.h>

#include <sys/poll.h>

/* ------------------------------------------------------------------ */
/* Property ID resolution — done once at init, cached in ghostcon_kms_t*/
/* ------------------------------------------------------------------ */

static bool
get_property_id(int fd, uint32_t obj_id, uint32_t obj_type,
                 const char *name, uint32_t *out_id)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props)
        return false;

    bool found = false;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        if (strcmp(prop->name, name) == 0) {
            *out_id = prop->prop_id;
            found = true;
        }
        drmModeFreeProperty(prop);
        if (found)
            break;
    }
    drmModeFreeObjectProperties(props);
    return found;
}

static bool
plane_is_primary(int fd, uint32_t plane_id)
{
    uint32_t type_prop_id;
    if (!get_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop_id))
        return false;

    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return false;

    bool is_primary = false;
    for (uint32_t i = 0; i < props->count_props; i++) {
        if (props->props[i] == type_prop_id) {
            is_primary = (props->prop_values[i] == DRM_PLANE_TYPE_PRIMARY);
            break;
        }
    }
    drmModeFreeObjectProperties(props);
    return is_primary;
}

/* ------------------------------------------------------------------ */
/* Resource discovery                                                  */
/* ------------------------------------------------------------------ */

static bool
find_connector_and_mode(int fd, drmModeRes *res,
                         uint32_t *out_connector_id, drmModeModeInfo *out_mode,
                         uint32_t *out_encoder_id)
{
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            *out_connector_id = conn->connector_id;
            *out_encoder_id = conn->encoder_id; /* may be 0 — caller falls back */
            /* Prefer the mode flagged DRM_MODE_TYPE_PREFERRED; else first. */
            *out_mode = conn->modes[0];
            for (int m = 0; m < conn->count_modes; m++) {
                if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                    *out_mode = conn->modes[m];
                    break;
                }
            }
            drmModeFreeConnector(conn);
            return true;
        }
        drmModeFreeConnector(conn);
    }
    return false;
}

static bool
find_crtc(int fd, drmModeRes *res, uint32_t encoder_id, uint32_t *out_crtc_id,
          int *out_crtc_index)
{
    if (encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, encoder_id);
        if (enc && enc->crtc_id) {
            uint32_t crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            for (int i = 0; i < res->count_crtcs; i++) {
                if (res->crtcs[i] == crtc_id) {
                    *out_crtc_id = crtc_id;
                    *out_crtc_index = i;
                    return true;
                }
            }
        }
        if (enc)
            drmModeFreeEncoder(enc);
    }

    /* No encoder currently bound (or its CRTC vanished) — fall back to
       the first CRTC in the resource list. Good enough for a single-
       display, single-connector setup; multi-head assignment is a
       later problem. */
    if (res->count_crtcs > 0) {
        *out_crtc_id = res->crtcs[0];
        *out_crtc_index = 0;
        return true;
    }
    return false;
}

static bool
find_primary_plane(int fd, int crtc_index, uint32_t *out_plane_id)
{
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes)
        return false;

    bool found = false;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane)
            continue;
        if ((plane->possible_crtcs & (1u << crtc_index)) &&
            plane_is_primary(fd, plane->plane_id)) {
            *out_plane_id = plane->plane_id;
            found = true;
        }
        drmModeFreePlane(plane);
        if (found)
            break;
    }
    drmModeFreePlaneResources(planes);
    return found;
}

/* ------------------------------------------------------------------ */
/* Init / deinit                                                       */
/* ------------------------------------------------------------------ */

bool
ghostcon_kms_init(ghostcon_kms_t *kms, int drm_fd)
{
    memset(kms, 0, sizeof(*kms));
    kms->fd = drm_fd;

    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "kms: atomic/universal-planes capability not available\n");
        return false;
    }

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) {
        fprintf(stderr, "kms: drmModeGetResources failed\n");
        return false;
    }

    uint32_t encoder_id = 0;
    if (!find_connector_and_mode(drm_fd, res, &kms->connector_id, &kms->mode, &encoder_id)) {
        fprintf(stderr, "kms: no connected connector with a usable mode\n");
        drmModeFreeResources(res);
        return false;
    }

    int crtc_index;
    if (!find_crtc(drm_fd, res, encoder_id, &kms->crtc_id, &crtc_index)) {
        fprintf(stderr, "kms: no usable CRTC\n");
        drmModeFreeResources(res);
        return false;
    }
    drmModeFreeResources(res);

    if (!find_primary_plane(drm_fd, crtc_index, &kms->plane_id)) {
        fprintf(stderr, "kms: no primary plane for CRTC %u\n", kms->crtc_id);
        return false;
    }

    kms->width = kms->mode.hdisplay;
    kms->height = kms->mode.vdisplay;

    bool ok = true;
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &kms->plane_props.crtc_id);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &kms->plane_props.fb_id);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &kms->plane_props.src_x);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &kms->plane_props.src_y);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &kms->plane_props.src_w);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &kms->plane_props.src_h);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &kms->plane_props.crtc_x);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &kms->plane_props.crtc_y);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &kms->plane_props.crtc_w);
    ok &= get_property_id(drm_fd, kms->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &kms->plane_props.crtc_h);
    ok &= get_property_id(drm_fd, kms->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &kms->crtc_props.mode_id);
    ok &= get_property_id(drm_fd, kms->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &kms->crtc_props.active);
    ok &= get_property_id(drm_fd, kms->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &kms->connector_props.crtc_id);
    if (!ok) {
        fprintf(stderr, "kms: failed to resolve one or more atomic property IDs\n");
        return false;
    }

    if (drmModeCreatePropertyBlob(drm_fd, &kms->mode, sizeof(kms->mode),
                                   &kms->mode_blob_id) != 0) {
        fprintf(stderr, "kms: drmModeCreatePropertyBlob failed\n");
        return false;
    }

    kms->gbm_dev = gbm_create_device(drm_fd);
    if (!kms->gbm_dev) {
        fprintf(stderr, "kms: gbm_create_device failed\n");
        return false;
    }

    return true;
}

void
ghostcon_kms_deinit(ghostcon_kms_t *kms)
{
    if (kms->current_bo)
        gbm_surface_release_buffer(kms->gbm_surf, kms->current_bo);
    if (kms->current_fb_id)
        drmModeRmFB(kms->fd, kms->current_fb_id);
    if (kms->mode_blob_id)
        drmModeDestroyPropertyBlob(kms->fd, kms->mode_blob_id);
    if (kms->gbm_surf)
        gbm_surface_destroy(kms->gbm_surf);
    if (kms->gbm_dev)
        gbm_device_destroy(kms->gbm_dev);
    memset(kms, 0, sizeof(*kms));
}

bool
ghostcon_kms_create_scanout_surface(ghostcon_kms_t *kms)
{
    kms->gbm_surf = gbm_surface_create(kms->gbm_dev, kms->width, kms->height,
                                        GBM_FORMAT_XRGB8888,
                                        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!kms->gbm_surf) {
        fprintf(stderr, "kms: gbm_surface_create (scanout) failed\n");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Framebuffer creation from a locked GBM buffer object                */
/* ------------------------------------------------------------------ */

static bool
fb_id_for_bo(int fd, struct gbm_bo *bo, uint32_t *out_fb_id)
{
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo), 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    return drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                          gbm_bo_get_format(bo), handles, strides, offsets,
                          out_fb_id, 0) == 0;
}

/* ------------------------------------------------------------------ */
/* Atomic commits                                                      */
/* ------------------------------------------------------------------ */

/* Disables every plane on this CRTC other than our own primary plane
   (kms->plane_id) -- a hardware cursor plane (or any other overlay) a
   previous DRM client (e.g. the DE's compositor, on the VT this one
   just took over from) left committed is NOT reset by drmSetMaster()/
   VT switching; atomic commits only ever change the properties they
   explicitly mention, so a plane nobody touches keeps showing whatever
   was last on it (found live: the DE session's mouse cursor sprite
   stayed visible, un-clickable, over ghostcon's own rendered frame).
   Deliberately blanket rather than cursor-plane-specific: matches this
   project's existing "check liveness/ownership, not the specific
   failure mode" philosophy (see PLAN.md's canary "Philosophy" section)
   -- catches any leftover overlay, not just the one instance already
   found. Best-effort: filtered to planes currently on OUR crtc_id (not
   just possible_crtcs, which is "could be", not "is") so a multi-
   monitor machine's other active display is never touched; a failed
   property lookup/add for one stray plane is silently skipped rather
   than failing the whole modeset over a cosmetic cleanup. Only needed
   once per modeset (acquire/reacquire), not per frame -- plane state
   sticks around exactly like the cursor sprite problem it's fixing. */
static void
disable_other_planes(ghostcon_kms_t *kms, drmModeAtomicReq *req)
{
    drmModePlaneRes *planes = drmModeGetPlaneResources(kms->fd);
    if (!planes)
        return;

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        uint32_t plane_id = planes->planes[i];
        if (plane_id == kms->plane_id)
            continue;

        uint32_t crtc_id_prop, fb_id_prop;
        if (!get_property_id(kms->fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &crtc_id_prop) ||
            !get_property_id(kms->fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &fb_id_prop))
            continue;

        drmModeObjectProperties *props =
            drmModeObjectGetProperties(kms->fd, plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props)
            continue;

        uint64_t current_crtc = 0;
        for (uint32_t p = 0; p < props->count_props; p++) {
            if (props->props[p] == crtc_id_prop) {
                current_crtc = props->prop_values[p];
                break;
            }
        }
        drmModeFreeObjectProperties(props);

        if (current_crtc == kms->crtc_id) {
            (void)drmModeAtomicAddProperty(req, plane_id, crtc_id_prop, 0);
            (void)drmModeAtomicAddProperty(req, plane_id, fb_id_prop, 0);
        }
    }
    drmModeFreePlaneResources(planes);
}

static bool
commit_frame(ghostcon_kms_t *kms, uint32_t fb_id, bool is_modeset, bool blocking)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return false;

    bool ok = true;
    if (is_modeset) {
        ok &= drmModeAtomicAddProperty(req, kms->connector_id,
                                        kms->connector_props.crtc_id, kms->crtc_id) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->crtc_id,
                                        kms->crtc_props.mode_id, kms->mode_blob_id) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->crtc_id,
                                        kms->crtc_props.active, 1) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.crtc_id, kms->crtc_id) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.src_x, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.src_y, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.src_w, (uint64_t)kms->width << 16) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.src_h, (uint64_t)kms->height << 16) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.crtc_x, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.crtc_y, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.crtc_w, kms->width) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->plane_id,
                                        kms->plane_props.crtc_h, kms->height) >= 0;

        disable_other_planes(kms, req);
    }
    ok &= drmModeAtomicAddProperty(req, kms->plane_id, kms->plane_props.fb_id, fb_id) >= 0;

    if (!ok) {
        drmModeAtomicFree(req);
        return false;
    }

    uint32_t flags = is_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
    if (!blocking)
        flags |= DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;

    /* user_data becomes page_flip_handler's `data` arg when the kernel
       delivers the completion event — it unconditionally dereferences
       this as a bool*, so passing NULL here crashed the process the
       moment any event actually arrived (found via a real segfault
       during live testing; earlier tests never got this far because
       they either never called page_flip at all, or always hit the
       poll timeout first). */
    int rv = drmModeAtomicCommit(kms->fd, req, flags, &kms->flip_pending);
    drmModeAtomicFree(req);
    if (rv != 0) {
        fprintf(stderr, "kms: drmModeAtomicCommit failed: %d\n", rv);
        return false;
    }
    return true;
}

static void
page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                   void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec;
    bool *pending = data;
    *pending = false;
}

static bool
wait_for_flip(ghostcon_kms_t *kms)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = page_flip_handler,
    };

    while (kms->flip_pending) {
        struct pollfd pfd = { .fd = kms->fd, .events = POLLIN };
        int rv = poll(&pfd, 1, 1000);
        if (rv <= 0)
            return false; /* timeout or error — leaves flip_pending set */
        drmHandleEvent(kms->fd, &ev);
    }
    return true;
}

bool
ghostcon_kms_modeset(ghostcon_kms_t *kms)
{
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(kms->gbm_surf);
    if (!bo) {
        fprintf(stderr, "kms: gbm_surface_lock_front_buffer failed\n");
        return false;
    }

    uint32_t fb_id;
    if (!fb_id_for_bo(kms->fd, bo, &fb_id)) {
        fprintf(stderr, "kms: drmModeAddFB2 failed\n");
        gbm_surface_release_buffer(kms->gbm_surf, bo);
        return false;
    }

    if (!commit_frame(kms, fb_id, /*is_modeset=*/true, /*blocking=*/true)) {
        drmModeRmFB(kms->fd, fb_id);
        gbm_surface_release_buffer(kms->gbm_surf, bo);
        return false;
    }

    kms->current_bo = bo;
    kms->current_fb_id = fb_id;
    return true;
}

bool
ghostcon_kms_page_flip(ghostcon_kms_t *kms)
{
    /* Best-effort pacing only, NOT a correctness gate — see below for
       why a timeout here must never leave a committed buffer untracked. */
    if (kms->flip_pending) {
        if (!wait_for_flip(kms))
            fprintf(stderr, "kms: previous page flip's completion event "
                            "never arrived (lost event?) — proceeding anyway\n");
        kms->flip_pending = false;
    }

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(kms->gbm_surf);
    if (!bo) {
        fprintf(stderr, "kms: gbm_surface_lock_front_buffer failed\n");
        return false;
    }

    uint32_t fb_id;
    if (!fb_id_for_bo(kms->fd, bo, &fb_id)) {
        fprintf(stderr, "kms: drmModeAddFB2 failed\n");
        gbm_surface_release_buffer(kms->gbm_surf, bo);
        return false;
    }

    if (!commit_frame(kms, fb_id, /*is_modeset=*/false, /*blocking=*/false)) {
        /* Genuinely rejected by the kernel — nothing changed on the
           display side, safe to just release what we allocated. */
        drmModeRmFB(kms->fd, fb_id);
        gbm_surface_release_buffer(kms->gbm_surf, bo);
        return false;
    }

    /* Once drmModeAtomicCommit succeeds, the kernel WILL apply this
       flip — a lost/delayed completion event only means we won't be
       notified promptly, it does NOT mean the flip didn't happen. An
       earlier version of this function treated "no event within 1s"
       as "nothing happened" and left this buffer completely untracked
       (not released, not adopted as current) rather than retiring the
       old one — meaning teardown could then destroy the GBM surface
       and drop DRM master while THIS buffer was still the one actively
       scanned out. That wedged the GPU/display badly enough to need a
       hard reboot during real-hardware testing. Adopting the new
       buffer as current unconditionally on a successful commit (and
       retiring the old one) is the actual correctness boundary; the
       completion event below is only used to pace the next flip. */
    if (kms->current_bo)
        gbm_surface_release_buffer(kms->gbm_surf, kms->current_bo);
    if (kms->current_fb_id)
        drmModeRmFB(kms->fd, kms->current_fb_id);
    kms->current_bo = bo;
    kms->current_fb_id = fb_id;

    kms->flip_pending = true;
    if (!wait_for_flip(kms))
        fprintf(stderr, "kms: this page flip's completion event never "
                        "arrived (lost event?) — buffer already adopted, "
                        "continuing\n");
    kms->flip_pending = false;

    return true;
}
