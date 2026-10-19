/* test_render
 *
 * Phase 1 item 2 smoke test for the GLES2 renderer.
 *
 * Headless: renders to a GBM surface backed by a DRM render node (no DRM master /
 * KMS scanout needed
 *
 * see core/egl.h).
 *
 * Feeds known text into a ghostcon_term_t, renders it via atlas+gles+machine,
 * reads back the framebuffer with glReadPixels, and verifies glyph pixels
 * actually differ from the background (i.e. something was drawn, not just a clear
 * color).
 *
 * The node defaults to /dev/dri/renderD128 and is overridable with
 * GHOSTCON_DRM_RENDER_NODE (core/render_node.h).
 *
 * When no render node is present -- the common case on a headless CI runner --
 * the test SKIPs (exit 77) rather than failing, so a GPU-less machine can still
 * run the rest of the suite.
 *
 * Does NOT exercise core/kms.c (real scanout)
 *
 * that needs a VT switch away from the live desktop session. See PLAN.md's Phase
 * 1 item 2 note for that follow-up.
 */

#include "ghostcon/core/egl.h"
#include "ghostcon/core/parse.h"
#include "ghostcon/core/render_node.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/term.h"

#include <GLES2/gl2.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COLS 20
#define ROWS 4
#define FONT_SIZE 16
#define ATLAS_DIM 512

int main(void)
{
    const char *node = ghostcon_render_node();
    if (access(node, R_OK | W_OK) != 0)
    {
        printf("SKIP: render node %s unavailable (%s)\n", node, strerror(errno));
        return 77;
    }

    ghostcon_atlas_t *atlas = ghostcon_atlas_create(NULL, NULL, NULL, NULL, FONT_SIZE, ATLAS_DIM);
    if (!atlas)
    {
        fprintf(stderr, "FAIL: ghostcon_atlas_create\n");
        return 1;
    }
    int cell_w, cell_h;
    ghostcon_atlas_cell_size(atlas, &cell_w, &cell_h);
    if (cell_w <= 0 || cell_h <= 0)
    {
        fprintf(stderr, "FAIL: bad cell size %dx%d\n", cell_w, cell_h);
        return 1;
    }
    printf("PASS: atlas created, cell size %dx%d\n", cell_w, cell_h);

    uint32_t vw = (uint32_t)(cell_w * COLS);
    uint32_t vh = (uint32_t)(cell_h * ROWS);

    ghostcon_egl_t egl;
    if (!ghostcon_egl_init(&egl, node, vw, vh))
    {
        fprintf(stderr, "FAIL: ghostcon_egl_init\n");
        return 1;
    }
    if (!ghostcon_egl_make_current(&egl))
    {
        fprintf(stderr, "FAIL: ghostcon_egl_make_current\n");
        return 1;
    }
    printf("PASS: EGL/GBM context on %s, viewport %ux%u\n", node, vw, vh);

    ghostcon_gles_t *gles = ghostcon_gles_create(vw, vh, true);
    if (!gles)
    {
        fprintf(stderr, "FAIL: ghostcon_gles_create\n");
        return 1;
    }
    printf("PASS: GLES program + atlas texture created\n");
    printf("%s: linear_blending active = %d (informational -- this project's own dev hardware "
           "confirmed it works, but isn't guaranteed everywhere)\n",
           ghostcon_gles_linear_blending_active(gles) ? "PASS" : "NOTE",
           ghostcon_gles_linear_blending_active(gles));

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, COLS, ROWS, 0))
    {
        fprintf(stderr, "FAIL: ghostcon_term_init\n");
        return 1;
    }
    const char *text = "Hello, ghostcon!";
    ghostcon_term_feed(&term, (const uint8_t *)text, strlen(text));

    ghostcon_gles_begin(gles, true, 0.0f, 0.0f, 0.0f);
    ghostcon_machine_render_dirty(&term.screen, atlas, gles, cell_w, cell_h, false);
    ghostcon_gles_sync_atlas(gles, atlas, true); /* force: fresh texture, nothing uploaded yet */
    ghostcon_gles_end(gles);

    size_t npixels = (size_t)vw * vh;
    uint8_t *pixels = malloc(npixels * 4);
    glReadPixels(0, 0, (GLsizei)vw, (GLsizei)vh, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        fprintf(stderr, "FAIL: GL error 0x%x\n", err);
        return 1;
    }

    /* Background was cleared to black; glyph coverage should paint
     * non-black pixels (default fg is not black) somewhere in the first
     * row's cell band.
     */
    size_t non_bg = 0;
    for (size_t i = 0; i < npixels; i++)
    {
        uint8_t r = pixels[i * 4 + 0];
        uint8_t g = pixels[i * 4 + 1];
        uint8_t b = pixels[i * 4 + 2];
        if (r > 16 || g > 16 || b > 16)
            non_bg++;
    }
    free(pixels);

    if (non_bg == 0)
    {
        fprintf(stderr, "FAIL: no glyph pixels rendered (frame is all background)\n");
        return 1;
    }
    printf("PASS: %zu non-background pixels rendered (text visibly drawn)\n", non_bg);

    const char *ppm_path = "/tmp/ghostcon-test-screenshot.ppm";
    if (!ghostcon_gles_screenshot_ppm(gles, ppm_path))
    {
        fprintf(stderr, "FAIL: ghostcon_gles_screenshot_ppm\n");
        return 1;
    }
    FILE *ppm = fopen(ppm_path, "rb");
    if (!ppm)
    {
        fprintf(stderr, "FAIL: screenshot PPM not readable\n");
        return 1;
    }
    unsigned pw = 0, ph = 0, maxv = 0;
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion) -- return count checked
    if (fscanf(ppm, "P6 %u %u %u", &pw, &ph, &maxv) != 3 ||
        pw != (unsigned)vw || ph != (unsigned)vh || maxv != 255)
    {
        fprintf(stderr, "FAIL: screenshot PPM header mismatch (%ux%u max=%u)\n", pw, ph, maxv);
        fclose(ppm);
        return 1;
    }
    fclose(ppm);
    printf("PASS: screenshot PPM written, %ux%u header matches viewport\n", pw, ph);

    ghostcon_egl_swap(&egl);
    printf("PASS: eglSwapBuffers\n");

    ghostcon_term_deinit(&term);
    ghostcon_gles_destroy(gles);
    ghostcon_egl_deinit(&egl);
    ghostcon_atlas_destroy(atlas);

    printf("ALL TESTS PASSED\n");
    return 0;
}
