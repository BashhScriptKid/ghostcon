#include "ghostcon/core/kms.h"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
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
plane_is_type(int fd, uint32_t plane_id, uint64_t want_type)
{
    uint32_t type_prop_id;
    if (!get_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type_prop_id))
        return false;

    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return false;

    bool matches = false;
    for (uint32_t i = 0; i < props->count_props; i++) {
        if (props->props[i] == type_prop_id) {
            matches = (props->prop_values[i] == want_type);
            break;
        }
    }
    drmModeFreeObjectProperties(props);
    return matches;
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
find_plane_of_type(int fd, int crtc_index, uint64_t want_type, uint32_t *out_plane_id)
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
            plane_is_type(fd, plane->plane_id, want_type)) {
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
/* Hardware cursor image                                               */
/* ------------------------------------------------------------------ */

/* Ceiling on top of whatever DRM_CAP_CURSOR_WIDTH/HEIGHT actually
   reports -- a sanity bound, not a real target size (kmscon's own
   video.h hardcodes 64 unconditionally, which this project copied at
   first without checking whether it undershot real hardware -- found
   live: this machine's actual cap is 256x256, and a raster cursor
   asset sized between the two, 96x96 from a real Xcursor theme,
   silently got center-cropped to fit the wrongly-small 64x64 canvas
   ghostcon was choosing instead of the real one available. Now uses
   the ACTUAL queried cap (clamped only by this ceiling, which exists
   so a driver reporting something absurd like 4096 doesn't allocate
   an oversized buffer for no reason) rather than second-guessing it
   down to kmscon's number. */
#define GHOSTCON_CURSOR_MAX_SIZE 256

/* Port of kmscon's generate_ibeam_cursor() (src/terminal.c) shape/
   algorithm -- a text I-beam (vertical stem + top/bottom serifs)
   rather than an arrow, matching how kmscon's own mouse cursor looks
   over a terminal. `rotate` (kmscon's screen-rotation support) is
   intentionally not ported -- ghostcon has no rotation feature to
   serve it.

   Draws the I-beam glyph (sized from `ibeam_h`, kmscon's own w/thk
   formula) CENTERED within a `canvas_w x canvas_h` ARGB8888 buffer
   (row-major, `stride` bytes per row -- may exceed canvas_w*4 for
   padding, per the dumb buffer's own pitch) rather than filling the
   buffer edge-to-edge -- found live: the buffer itself must be square
   (kmscon's own generate_ibeam_cursor() produces a narrower-than-tall
   w/h, which is fine for kmscon since it hands that non-square size
   straight to a legacy drmModeSetCursor() call; ghostcon's atomic-KMS
   SRC_W/SRC_H/CRTC_W/CRTC_H commit was rejected outright with EINVAL
   on this machine's amdgpu until the buffer itself became square --
   apparently a common cursor-plane HW restriction, not universal
   across drivers but safe to always honor). */
static void
draw_cursor_ibeam(uint8_t *pixels, uint32_t canvas_w, uint32_t canvas_h,
                   uint32_t stride, uint32_t ibeam_h,
                   uint32_t *out_off_x, uint32_t *out_off_y)
{
    memset(pixels, 0, (size_t)stride * canvas_h); /* fully transparent by default */

    int thk = 1 + (int)(ibeam_h / 16);
    uint32_t w = 2 * (ibeam_h / 6) + 3 * (uint32_t)thk;
    uint32_t h = ibeam_h;
    if (w > canvas_w) w = canvas_w;
    if (h > canvas_h) h = canvas_h;
    uint32_t off_x = (canvas_w - w) / 2;
    uint32_t off_y = (canvas_h - h) / 2;
    *out_off_x = off_x;
    *out_off_y = off_y;

    bool *shape = calloc(w, (size_t)h * sizeof(*shape));
    if (!shape)
        return;

    /* Vertical stem, centered (within the w x h glyph, not the canvas). */
    for (uint32_t y = (uint32_t)thk; y + (uint32_t)thk < h; y++)
        for (int i = 0; i < thk; i++)
            shape[(w - (uint32_t)thk) / 2 + y * w + (uint32_t)i] = true;

    /* Top and bottom serifs. */
    for (uint32_t x = (uint32_t)thk; x + (uint32_t)thk < w; x++) {
        for (int i = 0; i < thk; i++) {
            shape[w * (uint32_t)(i + thk) + x] = true;
            shape[(h - (uint32_t)i - 1 - (uint32_t)thk) * w + x] = true;
        }
    }

    /* White fill on the shape itself; a dark halo on neighboring
       pixels within `thk` distance, so the beam stays visible against
       any background color (matches kmscon's own outline approach).
       Offset into the canvas by (off_x, off_y) to center it. */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)(y + off_y) * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t *px = row + (x + off_x);
            if (shape[y * w + x]) {
                *px = 0xFFFFFFFFu; /* opaque white */
                continue;
            }
            bool near = false;
            for (int ny = (int)y - thk; ny <= (int)y + thk && !near; ny++) {
                for (int nx = (int)x - thk; nx <= (int)x + thk && !near; nx++) {
                    if (ny >= 0 && ny < (int)h && nx >= 0 && nx < (int)w &&
                        shape[(uint32_t)ny * w + (uint32_t)nx])
                        near = true;
                }
            }
            if (near)
                *px = 0xDC000000u; /* alpha 220, black -- kmscon's own outline color */
        }
    }
    free(shape);
}

/* Blits `pixels` (ARGB8888, w x h) into a canvas_w x canvas_h buffer,
   centered (transparent padding around it, same letterboxing the
   procedural I-beam already uses) -- clips if the source is larger
   than the canvas in either dimension, simplest reasonable behavior
   for a mismatch rather than scaling (a raster asset is expected to
   already be sized sensibly for a cursor; scaling adds complexity for
   a case that shouldn't come up in practice). */
static void
blit_cursor_image(uint8_t *dst_pixels, uint32_t canvas_w, uint32_t canvas_h, uint32_t stride,
                   const uint32_t *src, uint32_t src_w, uint32_t src_h,
                   uint32_t *out_off_x, uint32_t *out_off_y)
{
    memset(dst_pixels, 0, (size_t)stride * canvas_h);

    uint32_t copy_w = src_w < canvas_w ? src_w : canvas_w;
    uint32_t copy_h = src_h < canvas_h ? src_h : canvas_h;
    uint32_t off_x = (canvas_w - copy_w) / 2;
    uint32_t off_y = (canvas_h - copy_h) / 2;
    *out_off_x = off_x;
    *out_off_y = off_y;

    for (uint32_t y = 0; y < copy_h; y++) {
        uint32_t *dst_row = (uint32_t *)(dst_pixels + (size_t)(y + off_y) * stride);
        const uint32_t *src_row = src + (size_t)y * src_w;
        memcpy(dst_row + off_x, src_row, (size_t)copy_w * sizeof(uint32_t));
    }
}

/* Destroys the cursor FB/dumb buffer for one state, if any -- shared
   by ghostcon_kms_set_cursor_image() (recreating at a new image) and
   ghostcon_kms_deinit() (destroying all states' images). */
static void
destroy_cursor_image(ghostcon_kms_t *kms, ghostcon_cursor_state_t state)
{
    if (kms->cursor_fb_id[state]) {
        drmModeRmFB(kms->fd, kms->cursor_fb_id[state]);
        kms->cursor_fb_id[state] = 0;
    }
    if (kms->cursor_bo_handle[state]) {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = kms->cursor_bo_handle[state];
        drmIoctl(kms->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        kms->cursor_bo_handle[state] = 0;
    }
}

bool
ghostcon_kms_set_cursor_image(ghostcon_kms_t *kms, ghostcon_cursor_state_t state,
                               const uint32_t *pixels, uint32_t w, uint32_t h,
                               uint32_t hot_x, uint32_t hot_y,
                               unsigned int fallback_font_height)
{
    if (!kms->cursor_plane_id)
        return true; /* no cursor plane -- not an error */

    destroy_cursor_image(kms, state);
    if (state == kms->cursor_active_state) {
        /* Re-enable on the next move_cursor() call so the new FB
           actually gets committed -- its fast path only touches
           CRTC_X/CRTC_Y once already enabled. */
        kms->cursor_enabled = false;
    }

    /* The BUFFER is always a fixed square canvas (see this function's
       own doc comment in kms.h on why -- a non-square cursor plane
       commit was rejected outright with EINVAL on this machine).
       Sized to the smaller of the two queried DRM_CAP_CURSOR_WIDTH/
       HEIGHT caps (this machine: 256x256), clamped only by
       GHOSTCON_CURSOR_MAX_SIZE's sanity ceiling, not a hardcoded
       target -- see that macro's own doc comment for why. kms->cursor_w/h
       is shared by every state, only (re)computed
       once (the first time this is called with either plane just
       discovered), since it doesn't depend on which state or image is
       being set. */
    if (kms->cursor_w == 0 || kms->cursor_h == 0) {
        uint64_t cap_w = 0, cap_h = 0;
        if (drmGetCap(kms->fd, DRM_CAP_CURSOR_WIDTH, &cap_w) != 0 || cap_w == 0)
            cap_w = GHOSTCON_CURSOR_MAX_SIZE; /* DRM's own documented fallback when unqueryable */
        if (drmGetCap(kms->fd, DRM_CAP_CURSOR_HEIGHT, &cap_h) != 0 || cap_h == 0)
            cap_h = GHOSTCON_CURSOR_MAX_SIZE;
        uint32_t canvas = (uint32_t)(cap_w < cap_h ? cap_w : cap_h);
        if (canvas == 0 || canvas > GHOSTCON_CURSOR_MAX_SIZE)
            canvas = GHOSTCON_CURSOR_MAX_SIZE;
        kms->cursor_w = canvas;
        kms->cursor_h = canvas;
    }

    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = kms->cursor_w;
    creq.height = kms->cursor_h;
    creq.bpp = 32;
    if (drmIoctl(kms->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        fprintf(stderr, "kms: cursor DRM_IOCTL_MODE_CREATE_DUMB failed\n");
        return false;
    }
    kms->cursor_bo_handle[state] = creq.handle;

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(kms->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        fprintf(stderr, "kms: cursor DRM_IOCTL_MODE_MAP_DUMB failed\n");
        return false;
    }

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      kms->fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        fprintf(stderr, "kms: cursor buffer mmap failed\n");
        return false;
    }
    if (pixels) {
        uint32_t off_x, off_y;
        blit_cursor_image((uint8_t *)map, kms->cursor_w, kms->cursor_h, creq.pitch, pixels, w, h,
                           &off_x, &off_y);
        /* hot_x/hot_y are glyph-local (relative to `pixels`, w x h) --
           blit_cursor_image() letterboxes that glyph into the middle of
           the much larger fixed square canvas, so the stored hotspot
           must be shifted by that same offset. Without this, commit_
           cursor() anchors the CANVAS's top-left corner (mostly empty
           padding) at the pointer instead of the actual glyph pixels,
           which sit `off_x`/`off_y` further in -- exactly the "gap
           between where you're pointing and what's drawn" bug this
           fixes. Clamped to the copied region so a hot_x/hot_y at or
           past the glyph's own edge (rounding slop from main.c's
           scaling) can't push the anchor outside the glyph entirely. */
        kms->cursor_hot_x[state] = off_x + (hot_x < w ? hot_x : w - 1);
        kms->cursor_hot_y[state] = off_y + (hot_y < h ? hot_y : h - 1);
    } else {
        uint32_t ibeam_h = fallback_font_height > 8 ? fallback_font_height : 8;
        if (ibeam_h > kms->cursor_w) ibeam_h = kms->cursor_w;
        uint32_t off_x, off_y;
        draw_cursor_ibeam((uint8_t *)map, kms->cursor_w, kms->cursor_h, creq.pitch, ibeam_h,
                           &off_x, &off_y);
        /* Local hotspot is (0,0) -- top-left of the glyph itself --
           same letterbox-offset reasoning as the blit_cursor_image()
           branch above. */
        kms->cursor_hot_x[state] = off_x;
        kms->cursor_hot_y[state] = off_y;
    }
    munmap(map, creq.size);

    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t strides[4] = { creq.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(kms->fd, kms->cursor_w, kms->cursor_h, DRM_FORMAT_ARGB8888,
                       handles, strides, offsets, &kms->cursor_fb_id[state], 0) != 0) {
        fprintf(stderr, "kms: cursor drmModeAddFB2 failed\n");
        kms->cursor_fb_id[state] = 0;
        return false;
    }
    return true;
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

    if (!find_plane_of_type(drm_fd, crtc_index, DRM_PLANE_TYPE_PRIMARY, &kms->plane_id)) {
        fprintf(stderr, "kms: no primary plane for CRTC %u\n", kms->crtc_id);
        return false;
    }

    /* Not fatal if absent -- some drivers/CRTCs genuinely have no
       CURSOR-type plane, and the hardware mouse cursor just never
       renders in that case (ghostcon_kms_move_cursor() checks
       cursor_plane_id == 0 and no-ops). */
    if (!find_plane_of_type(drm_fd, crtc_index, DRM_PLANE_TYPE_CURSOR, &kms->cursor_plane_id))
        kms->cursor_plane_id = 0;

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

    /* Cursor plane property resolution is best-effort (see
       cursor_plane_id's own doc comment) -- a failure here just means
       the hardware cursor never renders, not a fatal init error,
       unlike the primary plane's identical-shaped block above. The
       cursor IMAGE itself isn't set up here -- it needs font_height
       (cell_h), which isn't known at this layer; the caller (core/
       main.c's acquire_display()) calls ghostcon_kms_set_cursor_shape()
       right after this returns. */
    if (kms->cursor_plane_id) {
        bool cok = true;
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &kms->cursor_plane_props.crtc_id);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &kms->cursor_plane_props.fb_id);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &kms->cursor_plane_props.src_x);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &kms->cursor_plane_props.src_y);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &kms->cursor_plane_props.src_w);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &kms->cursor_plane_props.src_h);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &kms->cursor_plane_props.crtc_x);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &kms->cursor_plane_props.crtc_y);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &kms->cursor_plane_props.crtc_w);
        cok &= get_property_id(drm_fd, kms->cursor_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &kms->cursor_plane_props.crtc_h);
        if (!cok) {
            fprintf(stderr, "kms: failed to resolve cursor plane property IDs, cursor disabled\n");
            kms->cursor_plane_id = 0;
        }
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
    for (int s = 0; s < GC_CURSOR_STATE_COUNT; s++)
        destroy_cursor_image(kms, (ghostcon_cursor_state_t)s);
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
   (kms->plane_id) and our own cursor plane (kms->cursor_plane_id, once
   this pass's hardware mouse cursor claims it -- see
   ghostcon_kms_move_cursor()) -- a hardware cursor plane (or any other
   overlay) a previous DRM client (e.g. the DE's compositor, on the VT
   this one just took over from) left committed is NOT reset by drmSetMaster()/
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
        if (plane_id == kms->plane_id || plane_id == kms->cursor_plane_id)
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

/* Shared by ghostcon_kms_move_cursor() and ghostcon_kms_set_cursor_state()
   -- both end up doing "commit the active state's plane geometry/
   position", just triggered by different things changing (position vs.
   which state is active). `force_full` re-sends CRTC_ID/FB_ID/SRC_x_y_w_h/
   CRTC_w_h (not just position) -- needed on the very first enable, and
   whenever the active state's FB has changed. */
static bool
commit_cursor(ghostcon_kms_t *kms, int x, int y, bool force_full)
{
    ghostcon_cursor_state_t state = kms->cursor_active_state;
    if (!kms->cursor_plane_id || !kms->cursor_fb_id[state])
        return true; /* no cursor plane/image available -- not an error */

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    /* Subtract the active state's own hotspot -- callers always pass
       the raw pointer position (see this function's own doc comment in
       kms.h), never a pre-adjusted one. CRTC_X/CRTC_Y on a DRM plane
       are a SIGNED property (DRM_MODE_PROP_SIGNED_RANGE) -- hardware
       explicitly supports a plane hanging partially off the top/left
       edge of the screen, the same way it already tolerates one
       extending past the bottom/right edge (never clamped there).
       Flooring this at 0 (an earlier version of this code did, to
       dodge unsigned underflow) pins the whole canvas -- including
       the mostly-empty letterboxed padding around the actual glyph --
       at the screen edge instead, which reads as a dead zone the
       glyph can never visually enter near the top-left corner (worse
       the larger the hotspot, e.g. once it includes the letterbox
       offset for a raster cursor much smaller than the canvas -- see
       PLAN.md). Signed math with no floor fixes that: a partially
       negative CRTC_X/Y just clips the same way an out-of-bounds
       positive one already does. */
    int32_t crtc_x = x - (int32_t)kms->cursor_hot_x[state];
    int32_t crtc_y = y - (int32_t)kms->cursor_hot_y[state];

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return false;

    bool ok = true;
    if (force_full || !kms->cursor_enabled) {
        /* First move (or a state switch): (re)set CRTC_ID/FB_ID and
           the fixed geometry (source rect + on-screen size never
           change again for a given state -- only position does,
           below). Deliberately deferred to first-move rather than
           done once at init, so the cursor stays invisible until the
           user actually moves the mouse (see kms.h's own doc comment
           on cursor_enabled). */
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.crtc_id, kms->crtc_id) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.fb_id, kms->cursor_fb_id[state]) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.src_x, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.src_y, 0) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.src_w, (uint64_t)kms->cursor_w << 16) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.src_h, (uint64_t)kms->cursor_h << 16) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.crtc_w, kms->cursor_w) >= 0;
        ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                        kms->cursor_plane_props.crtc_h, kms->cursor_h) >= 0;
    }
    /* (uint64_t)(int64_t)crtc_x -- NOT a plain (uint64_t) cast -- sign-
       extends a negative int32 through int64 first, so the property's
       lower 32 bits are the correct two's-complement negative value
       the kernel expects for a DRM_MODE_PROP_SIGNED_RANGE property.
       A direct (uint64_t)crtc_x would instead zero-extend, turning a
       small negative offset into a huge positive one. */
    ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                    kms->cursor_plane_props.crtc_x, (uint64_t)(int64_t)crtc_x) >= 0;
    ok &= drmModeAtomicAddProperty(req, kms->cursor_plane_id,
                                    kms->cursor_plane_props.crtc_y, (uint64_t)(int64_t)crtc_y) >= 0;

    if (!ok) {
        drmModeAtomicFree(req);
        return false;
    }

    /* Deliberately NOT DRM_MODE_ATOMIC_ALLOW_MODESET and no page-flip
       event request -- this only touches an already-enabled plane's
       position (or enables it for the first time, which doesn't need
       modeset permission either), so a plain synchronous commit
       completes immediately without waiting on vsync/flip-event
       bookkeeping, independent of commit_frame()'s per-content-frame
       path (see this function's own doc comment in kms.h for why that
       independence is the whole point of a hardware cursor plane). */
    int rv = drmModeAtomicCommit(kms->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if (rv != 0) {
        fprintf(stderr, "kms: cursor commit failed: %d\n", rv);
        return false;
    }
    kms->cursor_enabled = true;
    return true;
}

bool
ghostcon_kms_move_cursor(ghostcon_kms_t *kms, int x, int y)
{
    kms->cursor_last_x = x;
    kms->cursor_last_y = y;
    return commit_cursor(kms, x, y, /*force_full=*/false);
}

bool
ghostcon_kms_set_cursor_state(ghostcon_kms_t *kms, ghostcon_cursor_state_t state)
{
    if (state == kms->cursor_active_state)
        return true; /* already active -- no redundant commit */
    kms->cursor_active_state = state;
    /* Re-commit at the last known position (with the newly-active
       state's own hotspot applied) -- caller doesn't need to re-supply
       the position just to switch states. */
    return commit_cursor(kms, kms->cursor_last_x, kms->cursor_last_y, /*force_full=*/true);
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
