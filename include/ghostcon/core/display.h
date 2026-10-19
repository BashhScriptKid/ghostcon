#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ghostcon/core/cursor_image.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/term/screen.h"

/* Display Lifecycle & Hardware Presentation Backend
 *
 * Encapsulates DRM/KMS atomic modesetting, DRM Master management, GBM
 * buffer surfaces, EGL contexts, GLES 2.0 rendering, and hardware cursor
 * planes. Isolates Linux graphics plumbing from higher layers.
 */

typedef struct ghostcon_display ghostcon_display_t;

/* Hardware cursor configuration */
typedef struct
{
    char theme[256];
    char default_path[256];
    char link_path[256];
    char default_hot_pos[32];
    char link_hot_pos[32];
    float base_scale;
    bool scale_with_terminal;
} ghostcon_display_cursor_config_t;

/* Create an unacquired display backend object for a given DRM node and VT */
ghostcon_display_t *ghostcon_display_create(const char *drm_node, int vt_num, bool gamma_correct);
void ghostcon_display_destroy(ghostcon_display_t *disp);

/* Acquire DRM master, initialize KMS, GBM, EGL, and GLES. Returns true on
 * success, and populates out_width / out_height with screen resolution.
 */
bool ghostcon_display_acquire(ghostcon_display_t *disp, int cell_h, ghostcon_atlas_t *atlas,
                              uint32_t *out_width, uint32_t *out_height);

/* Release GLES, EGL, KMS, and drop DRM master synchronously for VT switch */
void ghostcon_display_release(ghostcon_display_t *disp);

/* Check if display is currently acquired */
bool ghostcon_display_is_acquired(const ghostcon_display_t *disp);

/* Get current dimensions */
uint32_t ghostcon_display_width(const ghostcon_display_t *disp);
uint32_t ghostcon_display_height(const ghostcon_display_t *disp);

/* Update cursor configuration and reload sprites on cursor plane */
void ghostcon_display_configure_cursor(ghostcon_display_t *disp,
                                       const ghostcon_display_cursor_config_t *cfg,
                                       int cell_h);
void ghostcon_display_reload_cursor(ghostcon_display_t *disp, int cell_h);

/* Move the hardware cursor sprite on the plane */
void ghostcon_display_move_cursor(ghostcon_display_t *disp, int x, int y);

/* Change active cursor sprite state (DEFAULT vs LINK) */
void ghostcon_display_set_cursor_state(ghostcon_display_t *disp, ghostcon_cursor_state_t state);
ghostcon_cursor_state_t ghostcon_display_get_cursor_state(const ghostcon_display_t *disp);

/* Update shader gamma correction */
void ghostcon_display_set_gamma_correct(ghostcon_display_t *disp, bool gamma_correct);

/* Sync font atlas into GLES texture */
void ghostcon_display_sync_atlas(ghostcon_display_t *disp, ghostcon_atlas_t *atlas, bool force);

/* Render a complete frame and commit to display (swap + page flip /
 * modeset). Returns true on success.
 */
bool ghostcon_display_render_frame(ghostcon_display_t *disp, ghostcon_screen_t *screen,
                                   ghostcon_atlas_t *atlas, int cell_w, int cell_h,
                                   bool ligatures, bool screenshot_requested);
