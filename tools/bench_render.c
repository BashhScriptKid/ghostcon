/*
 * bench_render -- measures full-canvas redraw throughput for
 * ghostcon's render pipeline (atlas + gles + machine), headless (a
 * DRM render node, no display/DRM master needed -- same setup
 * tests/test_render.c already uses, see core/egl.h's doc comment).
 *
 * Motivation: suspected slowness "fast scrolling" in a TUI -- every
 * visible row's content changing every frame is the worst case for
 * this renderer's "redraw everything dirty, one GLES draw call per
 * frame" design (see render/machine.c). This tool reproduces exactly
 * that worst case directly against the real render pipeline, with no
 * pty/VT/network in the loop, so the render cost is isolated from
 * everything else that could also be slow.
 *
 * Usage: bench_render [cols] [rows] [font_size] [frames]
 *   Defaults: 128x32 @ 24pt, 300 frames (matches a plausible real
 *   terminal size/font at typical desktop resolutions).
 */

#define _DEFAULT_SOURCE /* clock_gettime()/CLOCK_MONOTONIC under -std=c11 without this */

#include "ghostcon/core/egl.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/term.h"

#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ATLAS_DIM 1024
#define DRM_RENDER_NODE "/dev/dri/renderD128"
#define WARMUP_FRAMES 10
/* Enough scrollback that bouncing scroll_view() back and forth for
   the whole benchmark run never runs out of history to scroll into --
   each "frame" scrolls by 1 line, so this needs to comfortably exceed
   half of any plausible --frames value. */
#define SCROLLBACK_LINES 2000

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Fills the terminal with SCROLLBACK_LINES lines of varied, colorful
   content -- exercises the atlas's real glyph rasterization path
   (many distinct codepoints, not just one repeated character) and the
   color-resolution path (machine.c's resolve_colors()), same as real
   terminal output would, rather than a degenerate all-blank or
   all-one-glyph case that wouldn't stress the atlas/GLES paths
   realistically. */
static void
fill_scrollback(ghostcon_term_t *term, uint16_t cols)
{
    char *line = malloc((size_t)cols * 32 + 64);
    for (int i = 0; i < SCROLLBACK_LINES; i++) {
        size_t off = 0;
        /* Cycle through 6 SGR colors and a mix of letters/digits/
           punctuation so the atlas has real variety to rasterize. */
        int color = 31 + (i % 6);
        off += (size_t)snprintf(line + off, cols * 32, "\x1b[%dm", color);
        for (uint16_t c = 0; c < cols; c++) {
            char ch = (char)('!' + ((i * 7 + c * 13) % 94)); /* printable ASCII range */
            line[off++] = ch;
        }
        off += (size_t)snprintf(line + off, 64, "\x1b[0m\r\n");
        ghostcon_term_feed(term, (const uint8_t *)line, off);
    }
    free(line);
}

int
main(int argc, char **argv)
{
    uint16_t cols = argc > 1 ? (uint16_t)atoi(argv[1]) : 128;
    uint16_t rows = argc > 2 ? (uint16_t)atoi(argv[2]) : 32;
    int font_size = argc > 3 ? atoi(argv[3]) : 24;
    int frames = argc > 4 ? atoi(argv[4]) : 300;

    if (cols == 0 || rows == 0 || font_size <= 0 || frames <= 0) {
        fprintf(stderr, "usage: %s [cols] [rows] [font_size] [frames]\n", argv[0]);
        return 1;
    }

    ghostcon_atlas_t *atlas = ghostcon_atlas_create(NULL, NULL, font_size, ATLAS_DIM);
    if (!atlas) {
        fprintf(stderr, "FAIL: ghostcon_atlas_create\n");
        return 1;
    }
    int cell_w, cell_h;
    ghostcon_atlas_cell_size(atlas, &cell_w, &cell_h);
    uint32_t vw = (uint32_t)(cell_w * cols);
    uint32_t vh = (uint32_t)(cell_h * rows);

    ghostcon_egl_t egl;
    if (!ghostcon_egl_init(&egl, DRM_RENDER_NODE, vw, vh)) {
        fprintf(stderr, "FAIL: ghostcon_egl_init on %s\n", DRM_RENDER_NODE);
        return 1;
    }
    if (!ghostcon_egl_make_current(&egl)) {
        fprintf(stderr, "FAIL: ghostcon_egl_make_current\n");
        return 1;
    }

    ghostcon_gles_t *gles = ghostcon_gles_create(vw, vh);
    if (!gles) {
        fprintf(stderr, "FAIL: ghostcon_gles_create\n");
        return 1;
    }

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, SCROLLBACK_LINES)) {
        fprintf(stderr, "FAIL: ghostcon_term_init\n");
        return 1;
    }

    printf("bench_render: %ux%u cells (%ux%u px), font_size=%d, %d frames\n",
           cols, rows, vw, vh, font_size, frames);
    printf("filling %d lines of scrollback...\n", SCROLLBACK_LINES);
    fill_scrollback(&term, cols);

    /* Force one full sync now (uploads every glyph fill_scrollback()
       touched) so it doesn't land inside the timed loop as a one-time
       cost masquerading as steady-state frame time. */
    ghostcon_gles_begin(gles, true, 0.0f, 0.0f, 0.0f);
    ghostcon_machine_render_dirty(&term.screen, atlas, gles, cell_w, cell_h);
    ghostcon_gles_sync_atlas(gles, atlas, true);
    ghostcon_gles_end(gles);
    ghostcon_egl_swap(&egl);
    glFinish();

    /* Warmup: first few real frames often pay one-time driver/shader
       JIT costs not representative of steady-state performance. */
    int dir = -1;
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        ghostcon_screen_scroll_view(&term.screen, dir);
        dir = -dir;
        ghostcon_gles_begin(gles, true, 0.0f, 0.0f, 0.0f);
        ghostcon_machine_render_dirty(&term.screen, atlas, gles, cell_w, cell_h);
        ghostcon_machine_render_cursor(&term.screen, gles, cell_w, cell_h);
        ghostcon_gles_sync_atlas(gles, atlas, false);
        ghostcon_gles_end(gles);
        ghostcon_egl_swap(&egl);
        glFinish();
    }

    double *frame_ms = malloc((size_t)frames * sizeof(double));
    double total_ms = 0.0, min_ms = 1e9, max_ms = 0.0;

    for (int i = 0; i < frames; i++) {
        /* scroll_view() marks the WHOLE visible grid dirty (see its
           own doc comment) -- exactly the worst case a fast-scrolling
           TUI hits every frame: every visible cell's content changed,
           nothing can be skipped via dirty-region tracking. */
        ghostcon_screen_scroll_view(&term.screen, dir);
        dir = -dir;

        double t0 = now_ms();
        ghostcon_gles_begin(gles, true, 0.0f, 0.0f, 0.0f);
        ghostcon_machine_render_dirty(&term.screen, atlas, gles, cell_w, cell_h);
        ghostcon_machine_render_cursor(&term.screen, gles, cell_w, cell_h);
        ghostcon_machine_render_selection(&term.screen, gles, cell_w, cell_h);
        ghostcon_gles_sync_atlas(gles, atlas, false);
        ghostcon_gles_end(gles);
        ghostcon_egl_swap(&egl);
        glFinish(); /* block until the GPU actually finished this frame's work */
        double dt = now_ms() - t0;

        frame_ms[i] = dt;
        total_ms += dt;
        if (dt < min_ms) min_ms = dt;
        if (dt > max_ms) max_ms = dt;
    }

    /* Simple descending sort for percentiles -- frames is small
       (hundreds), O(n^2) insertion sort is plenty fast enough here and
       keeps this file dependency-free. */
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

    printf("\n--- results (%d frames, glFinish()-synced, GPU-inclusive) ---\n", frames);
    printf("avg: %6.2f ms  (%6.1f fps)\n", avg_ms, 1000.0 / avg_ms);
    printf("min: %6.2f ms  (%6.1f fps)\n", min_ms, 1000.0 / min_ms);
    printf("p50: %6.2f ms  (%6.1f fps)\n", p50, 1000.0 / p50);
    printf("p95: %6.2f ms  (%6.1f fps)\n", p95, 1000.0 / p95);
    printf("p99: %6.2f ms  (%6.1f fps)\n", p99, 1000.0 / p99);
    printf("max: %6.2f ms  (%6.1f fps)\n", max_ms, 1000.0 / max_ms);
    printf("\nFor reference: 16.7ms/frame = 60fps, 33.3ms/frame = 30fps.\n");

    free(frame_ms);
    ghostcon_term_deinit(&term);
    ghostcon_gles_destroy(gles);
    ghostcon_egl_deinit(&egl);
    ghostcon_atlas_destroy(atlas);
    return 0;
}
