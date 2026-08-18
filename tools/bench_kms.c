/*
 * bench_kms -- measures the REAL DRM atomic-commit/page-flip pacing
 * cost (core/kms.c's ghostcon_kms_modeset()/ghostcon_kms_page_flip(),
 * the exact sequence core/main.c's render_frame() uses every frame),
 * isolated from GLES render cost (already covered by bench_render,
 * which showed it's nowhere near the bottleneck).
 *
 * Unlike bench_render/bench_parse/bench_ptyserv, this ONE needs real
 * DRM master on the primary node -- it actually modesets and scans
 * out to the physical display, so it can only run while nothing else
 * (ghostcon-core, a desktop compositor) already holds master on the
 * same card. Meant to be run with the real ghostcon.service stopped
 * for the duration.
 *
 * Renders a trivial solid-color clear per frame (no atlas/glyphs --
 * that cost is already isolated by bench_render) and times
 * ghostcon_egl_swap() + ghostcon_kms_page_flip() together, which is
 * what actually paces frame delivery: page_flip() waits for the
 * PREVIOUS flip's completion event before submitting the next one, so
 * steady-state timing here should track the display's real vsync
 * interval (e.g. ~16.7ms at 60Hz) if the KMS commit path is healthy.
 *
 * Usage: bench_kms [drm_node] [frames]
 *   Defaults: /dev/dri/card1, 300 frames.
 */

#define _DEFAULT_SOURCE

#include "ghostcon/core/egl.h"
#include "ghostcon/core/kms.h"
#include "ghostcon/render/gles.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>

#define DRM_MASTER_RETRIES 20
#define DRM_MASTER_RETRY_DELAY_US 100000

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int
main(int argc, char **argv)
{
    const char *drm_node = argc > 1 ? argv[1] : "/dev/dri/card1";
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    if (frames <= 0) {
        fprintf(stderr, "usage: %s [drm_node] [frames]\n", argv[0]);
        return 1;
    }

    int drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "FAIL: open %s: %m\n", drm_node);
        return 1;
    }

    bool have_master = false;
    int last_errno = 0;
    for (int i = 0; i < DRM_MASTER_RETRIES && !have_master; i++) {
        if (drmSetMaster(drm_fd) == 0)
            have_master = true;
        else {
            last_errno = errno;
            usleep(DRM_MASTER_RETRY_DELAY_US);
        }
    }
    if (!have_master) {
        /* Report the REAL errno rather than guessing at a cause --
           found live: this used to unconditionally claim "something
           else holds master," which was flat wrong (fuser showed
           nothing holding the device) and sent troubleshooting in the
           wrong direction entirely. */
        fprintf(stderr, "FAIL: drmSetMaster on %s: %s (errno %d)\n",
                drm_node, strerror(last_errno), last_errno);
        close(drm_fd);
        return 1;
    }
    printf("bench_kms: acquired DRM master on %s\n", drm_node);

    ghostcon_kms_t kms;
    if (!ghostcon_kms_init(&kms, drm_fd) || !ghostcon_kms_create_scanout_surface(&kms)) {
        fprintf(stderr, "FAIL: kms init/scanout surface\n");
        drmDropMaster(drm_fd);
        close(drm_fd);
        return 1;
    }
    printf("bench_kms: scanout surface %ux%u\n", kms.width, kms.height);

    ghostcon_egl_t egl;
    if (!ghostcon_egl_init_with_gbm(&egl, kms.gbm_dev, kms.gbm_surf, kms.width, kms.height) ||
        !ghostcon_egl_make_current(&egl)) {
        fprintf(stderr, "FAIL: egl init\n");
        drmDropMaster(drm_fd);
        close(drm_fd);
        return 1;
    }

    ghostcon_gles_t *gles = ghostcon_gles_create(kms.width, kms.height);
    if (!gles) {
        fprintf(stderr, "FAIL: gles create\n");
        drmDropMaster(drm_fd);
        close(drm_fd);
        return 1;
    }

    /* First frame: blocking modeset (same as core/main.c's
       render_frame() does the very first time -- app->did_modeset). */
    ghostcon_gles_begin(gles, true, 0.1f, 0.1f, 0.3f);
    ghostcon_gles_end(gles);
    if (!ghostcon_egl_swap(&egl) || !ghostcon_kms_modeset(&kms)) {
        fprintf(stderr, "FAIL: initial modeset\n");
        drmDropMaster(drm_fd);
        close(drm_fd);
        return 1;
    }
    printf("bench_kms: initial modeset committed, starting %d-frame timed run "
           "(display will show a plain color-cycling test pattern)\n", frames);

    double *frame_ms = malloc((size_t)frames * sizeof(double));
    double total_ms = 0.0, min_ms = 1e9, max_ms = 0.0;

    for (int i = 0; i < frames; i++) {
        /* Cycle the clear color so it's visually obvious on the real
           display that frames are actually changing, not just
           re-committing an identical buffer the driver might
           special-case. */
        float t = (float)i / (float)frames;
        ghostcon_gles_begin(gles, true, t, 1.0f - t, 0.5f);
        ghostcon_gles_end(gles);

        double t0 = now_ms();
        bool ok = ghostcon_egl_swap(&egl) && ghostcon_kms_page_flip(&kms);
        double dt = now_ms() - t0;
        if (!ok) {
            fprintf(stderr, "FAIL: frame %d: swap/page_flip failed\n", i);
            break;
        }

        frame_ms[i] = dt;
        total_ms += dt;
        if (dt < min_ms) min_ms = dt;
        if (dt > max_ms) max_ms = dt;
    }

    for (int i = 1; i < frames; i++) {
        double key = frame_ms[i];
        int j = i - 1;
        while (j >= 0 && frame_ms[j] > key) {
            frame_ms[j + 1] = frame_ms[j];
            j--;
        }
        frame_ms[j + 1] = key;
    }
    double p50 = frame_ms[frames / 2];
    double p95 = frame_ms[(int)((double)frames * 0.95)];
    double p99 = frame_ms[(int)((double)frames * 0.99)];
    double avg_ms = total_ms / frames;

    printf("\n--- results (%d frames, real DRM commit + page-flip wait) ---\n", frames);
    printf("avg: %6.2f ms  (%6.1f fps)\n", avg_ms, 1000.0 / avg_ms);
    printf("min: %6.2f ms  (%6.1f fps)\n", min_ms, 1000.0 / min_ms);
    printf("p50: %6.2f ms  (%6.1f fps)\n", p50, 1000.0 / p50);
    printf("p95: %6.2f ms  (%6.1f fps)\n", p95, 1000.0 / p95);
    printf("p99: %6.2f ms  (%6.1f fps)\n", p99, 1000.0 / p99);
    printf("max: %6.2f ms  (%6.1f fps)\n", max_ms, 1000.0 / max_ms);
    printf("\nFor reference: 16.7ms/frame = 60fps (typical vsync interval --\n"
           "steady timing AROUND there is healthy pacing, not a bug; wildly\n"
           "inconsistent or much-larger-than-vsync timing indicates a real\n"
           "problem in the commit path).\n");

    free(frame_ms);
    ghostcon_gles_destroy(gles);
    ghostcon_egl_deinit(&egl);
    ghostcon_kms_deinit(&kms);
    drmDropMaster(drm_fd);
    close(drm_fd);
    return 0;
}
