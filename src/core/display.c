#define _DEFAULT_SOURCE

#include "ghostcon/core/display.h"
#include "ghostcon/core/log.h"
#include "ghostcon/core/diag.h"
#include "ghostcon/core/egl.h"
#include "ghostcon/core/kms.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#define CURSOR_SCALE_REFERENCE_CELL_H 22
#define CURSOR_BASE_SCALE_CELL_RATIO 1.25f
#define DRM_MASTER_RETRIES 30
#define DRM_MASTER_RETRY_DELAY_US 100000

static const char *CURSOR_THEME_NAMES_DEFAULT[] = { "text", "xterm" };
static const char *CURSOR_THEME_NAMES_LINK[] = { "pointer", "hand2", "hand1" };

struct ghostcon_display {
    char drm_node[256];
    int vt_num;
    bool gamma_correct;

    int drm_fd;
    bool have_master;
    bool did_modeset;
    bool acquired;

    ghostcon_kms_t kms;
    ghostcon_egl_t egl;
    ghostcon_gles_t *gles;

    ghostcon_display_cursor_config_t cursor_cfg;
    ghostcon_cursor_state_t cursor_state;
};

static void
load_cursor_state(ghostcon_display_t *disp, ghostcon_cursor_state_t state,
                  const char *override_path, const char *hot_pos_str,
                  const char **theme_names, size_t n_theme_names, int cell_h)
{
    uint32_t w = 0, h = 0, hot_x = 0, hot_y = 0;
    uint32_t *pixels = NULL;
    bool have_real_hotspot = false;

    if (override_path && *override_path)
        pixels = ghostcon_cursor_load_bmp(override_path, &w, &h);

    if (!pixels && disp->cursor_cfg.theme[0]) {
        for (size_t i = 0; i < n_theme_names && !pixels; i++) {
            pixels = ghostcon_cursor_load_xcursor(disp->cursor_cfg.theme, theme_names[i],
                                                  &w, &h, &hot_x, &hot_y);
            if (pixels)
                have_real_hotspot = true;
        }
    }

    if (hot_pos_str && *hot_pos_str) {
        unsigned int cfg_hot_x, cfg_hot_y;
        if (sscanf(hot_pos_str, "%u,%u", &cfg_hot_x, &cfg_hot_y) == 2) {
            hot_x = cfg_hot_x;
            hot_y = cfg_hot_y;
            have_real_hotspot = true;
        } else {
            fprintf(stderr, "ghostcon-display: invalid cursor hot_pos \"%s\" -- expected \"x,y\"\n",
                    hot_pos_str);
        }
    }

    if (pixels && w > 0 && h > 0) {
        uint32_t cropped_w, cropped_h;
        uint32_t *cropped = ghostcon_cursor_crop_to_content(pixels, w, h, &cropped_w, &cropped_h,
                                                            &hot_x, &hot_y);
        if (cropped) {
            free(pixels);
            pixels = cropped;
            w = cropped_w;
            h = cropped_h;
        }
    }

    if (pixels && w > 0 && h > 0) {
        int ref_cell_h = disp->cursor_cfg.scale_with_terminal ? cell_h
                                                              : CURSOR_SCALE_REFERENCE_CELL_H;
        if (ref_cell_h <= 0)
            ref_cell_h = CURSOR_SCALE_REFERENCE_CELL_H;

        float target_h_f = disp->cursor_cfg.base_scale * CURSOR_BASE_SCALE_CELL_RATIO * (float)ref_cell_h;
        float scale = target_h_f / (float)h;

        uint32_t target_w = (uint32_t)((float)w * scale + 0.5f);
        uint32_t target_h = (uint32_t)(target_h_f + 0.5f);
        if (target_w < 1) target_w = 1;
        if (target_h < 1) target_h = 1;

        if (target_w != w || target_h != h) {
            uint32_t *scaled = ghostcon_cursor_scale(pixels, w, h, target_w, target_h);
            if (scaled) {
                hot_x = (uint32_t)((float)hot_x * scale + 0.5f);
                hot_y = (uint32_t)((float)hot_y * scale + 0.5f);
                free(pixels);
                pixels = scaled;
                w = target_w;
                h = target_h;
            }
        }

        if (!have_real_hotspot) {
            hot_x = w / 2;
            hot_y = h / 2;
        }
    }

    ghostcon_kms_set_cursor_image(&disp->kms, state, pixels, w, h, hot_x, hot_y,
                                  (unsigned int)cell_h);
    free(pixels);
}

void
ghostcon_display_reload_cursor(ghostcon_display_t *disp, int cell_h)
{
    if (!disp || !disp->acquired)
        return;

    load_cursor_state(disp, GC_CURSOR_STATE_DEFAULT, disp->cursor_cfg.default_path,
                      disp->cursor_cfg.default_hot_pos,
                      CURSOR_THEME_NAMES_DEFAULT,
                      sizeof(CURSOR_THEME_NAMES_DEFAULT) / sizeof(CURSOR_THEME_NAMES_DEFAULT[0]),
                      cell_h);
    load_cursor_state(disp, GC_CURSOR_STATE_LINK, disp->cursor_cfg.link_path,
                      disp->cursor_cfg.link_hot_pos,
                      CURSOR_THEME_NAMES_LINK,
                      sizeof(CURSOR_THEME_NAMES_LINK) / sizeof(CURSOR_THEME_NAMES_LINK[0]),
                      cell_h);
}

void
ghostcon_display_configure_cursor(ghostcon_display_t *disp,
                                  const ghostcon_display_cursor_config_t *cfg,
                                  int cell_h)
{
    if (!disp || !cfg)
        return;

    disp->cursor_cfg = *cfg;
    if (disp->acquired)
        ghostcon_display_reload_cursor(disp, cell_h);
}

ghostcon_display_t *
ghostcon_display_create(const char *drm_node, int vt_num, bool gamma_correct)
{
    ghostcon_display_t *disp = calloc(1, sizeof(*disp));
    if (!disp)
        return NULL;

    snprintf(disp->drm_node, sizeof(disp->drm_node), "%s", drm_node ? drm_node : "/dev/dri/card0");
    disp->vt_num = vt_num;
    disp->gamma_correct = gamma_correct;
    disp->drm_fd = -1;
    disp->cursor_cfg.base_scale = 1.0f;
    disp->cursor_cfg.scale_with_terminal = true;

    return disp;
}

void
ghostcon_display_destroy(ghostcon_display_t *disp)
{
    if (!disp)
        return;

    ghostcon_display_release(disp);
    if (disp->drm_fd >= 0) {
        close(disp->drm_fd);
        disp->drm_fd = -1;
    }
    free(disp);
}

bool
ghostcon_display_is_acquired(const ghostcon_display_t *disp)
{
    return disp && disp->acquired;
}

uint32_t
ghostcon_display_width(const ghostcon_display_t *disp)
{
    return disp && disp->acquired ? disp->kms.width : 0;
}

uint32_t
ghostcon_display_height(const ghostcon_display_t *disp)
{
    return disp && disp->acquired ? disp->kms.height : 0;
}

void
ghostcon_display_move_cursor(ghostcon_display_t *disp, int x, int y)
{
    if (disp && disp->acquired)
        ghostcon_kms_move_cursor(&disp->kms, x, y);
}

void
ghostcon_display_set_cursor_state(ghostcon_display_t *disp, ghostcon_cursor_state_t state)
{
    if (!disp || !disp->acquired)
        return;

    if (state != disp->cursor_state) {
        disp->cursor_state = state;
        ghostcon_kms_set_cursor_state(&disp->kms, state);
        GC_LOG(cursor, "state -> %s",
                state == GC_CURSOR_STATE_LINK ? "LINK" : "DEFAULT");
    }
}

ghostcon_cursor_state_t
ghostcon_display_get_cursor_state(const ghostcon_display_t *disp)
{
    return disp ? disp->cursor_state : GC_CURSOR_STATE_DEFAULT;
}

void
ghostcon_display_set_gamma_correct(ghostcon_display_t *disp, bool gamma_correct)
{
    if (!disp)
        return;
    disp->gamma_correct = gamma_correct;
    if (disp->gles)
        ghostcon_gles_set_gamma_correct(disp->gles, gamma_correct);
}

void
ghostcon_display_sync_atlas(ghostcon_display_t *disp, ghostcon_atlas_t *atlas, bool force)
{
    if (disp && disp->gles && atlas)
        ghostcon_gles_sync_atlas(disp->gles, atlas, force);
}

void
ghostcon_display_release(ghostcon_display_t *disp)
{
    if (!disp || !disp->acquired)
        return;

    if (disp->gles) {
        ghostcon_gles_destroy(disp->gles);
        disp->gles = NULL;
    }
    ghostcon_egl_deinit(&disp->egl);
    ghostcon_kms_deinit(&disp->kms);

    if (disp->have_master && disp->drm_fd >= 0)
        drmDropMaster(disp->drm_fd);

    disp->have_master = false;
    disp->did_modeset = false;
    disp->acquired = false;

    ghostcon_diag_log_warning("vt %d: display released", disp->vt_num);
}

bool
ghostcon_display_acquire(ghostcon_display_t *disp, int cell_h, ghostcon_atlas_t *atlas,
                         uint32_t *out_width, uint32_t *out_height)
{
    if (!disp)
        return false;

    if (disp->drm_fd >= 0) {
        close(disp->drm_fd);
        disp->drm_fd = -1;
    }

    disp->drm_fd = open(disp->drm_node, O_RDWR | O_CLOEXEC);
    if (disp->drm_fd < 0) {
        fprintf(stderr, "ghostcon-display: open %s: %s\n", disp->drm_node, strerror(errno));
        return false;
    }

    for (int i = 0; i < DRM_MASTER_RETRIES && !disp->have_master; i++) {
        if (drmSetMaster(disp->drm_fd) == 0)
            disp->have_master = true;
        else {
            GC_LOG(drm, "master busy, retry %d/%d", i + 1, DRM_MASTER_RETRIES);
            usleep(DRM_MASTER_RETRY_DELAY_US);
        }
    }
    if (!disp->have_master) {
        fprintf(stderr, "ghostcon-display: drmSetMaster failed: %s\n", strerror(errno));
        close(disp->drm_fd);
        disp->drm_fd = -1;
        return false;
    }

    if (!ghostcon_kms_init(&disp->kms, disp->drm_fd) ||
        !ghostcon_kms_create_scanout_surface(&disp->kms)) {
        fprintf(stderr, "ghostcon-display: kms init failed\n");
        goto fail;
    }

    disp->acquired = true;
    ghostcon_display_reload_cursor(disp, cell_h);

    if (!ghostcon_egl_init_with_gbm(&disp->egl, disp->kms.gbm_dev, disp->kms.gbm_surf,
                                    disp->kms.width, disp->kms.height) ||
        !ghostcon_egl_make_current(&disp->egl)) {
        fprintf(stderr, "ghostcon-display: egl init failed\n");
        goto fail;
    }

    disp->gles = ghostcon_gles_create(disp->kms.width, disp->kms.height, disp->gamma_correct);
    if (!disp->gles) {
        fprintf(stderr, "ghostcon-display: gles init failed\n");
        goto fail;
    }

    if (atlas)
        ghostcon_gles_sync_atlas(disp->gles, atlas, true);
    ghostcon_gles_set_gamma_correct(disp->gles, disp->gamma_correct);

    disp->did_modeset = false;
    if (out_width)  *out_width = disp->kms.width;
    if (out_height) *out_height = disp->kms.height;

    ghostcon_diag_log_warning("vt %d: display acquired (%ux%u)",
                              disp->vt_num, disp->kms.width, disp->kms.height);
    return true;

fail:
    if (disp->gles) {
        ghostcon_gles_destroy(disp->gles);
        disp->gles = NULL;
    }
    ghostcon_egl_deinit(&disp->egl);
    ghostcon_kms_deinit(&disp->kms);
    if (disp->drm_fd >= 0) {
        if (disp->have_master)
            drmDropMaster(disp->drm_fd);
        close(disp->drm_fd);
        disp->drm_fd = -1;
    }
    disp->have_master = false;
    disp->acquired = false;
    return false;
}

bool
ghostcon_display_render_frame(ghostcon_display_t *disp, ghostcon_screen_t *screen,
                              ghostcon_atlas_t *atlas, int cell_w, int cell_h,
                              bool ligatures, bool screenshot_requested)
{
    if (!disp || !disp->acquired || !disp->gles || !screen || !atlas)
        return false;

    GhosttyColorRgb bg_rgb = screen->palette.bg_default;
    float bg[3] = {
        (float)bg_rgb.r / 255.0f,
        (float)bg_rgb.g / 255.0f,
        (float)bg_rgb.b / 255.0f,
    };

    ghostcon_gles_begin(disp->gles, true, bg[0], bg[1], bg[2]);
    ghostcon_machine_render_images(screen, disp->gles, cell_w, cell_h);
    ghostcon_machine_render_dirty(screen, atlas, disp->gles, cell_w, cell_h, ligatures);
    ghostcon_machine_render_cursor(screen, disp->gles, cell_w, cell_h);
    ghostcon_machine_render_selection(screen, disp->gles, cell_w, cell_h);
    ghostcon_gles_sync_atlas(disp->gles, atlas, false);
    ghostcon_gles_end(disp->gles);

    if (screenshot_requested) {
        if (!ghostcon_gles_screenshot_ppm(disp->gles, "/tmp/ghostcon-screenshot.ppm"))
            fprintf(stderr, "ghostcon-display: screenshot requested but failed\n");
    }

    if (!ghostcon_egl_swap(&disp->egl))
        return false;

    if (!disp->did_modeset) {
        if (!ghostcon_kms_modeset(&disp->kms))
            return false;
        disp->did_modeset = true;
    } else {
        if (!ghostcon_kms_page_flip(&disp->kms))
            return false;
    }

    return true;
}
