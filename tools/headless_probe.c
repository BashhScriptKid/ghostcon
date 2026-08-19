/*
 * headless_probe -- one-off diagnostic tool (not registered as a
 * meson test/install target on purpose): runs a real child program
 * (e.g. nmtui) under a pty, feeds its output through the real term
 * pipeline, and renders one frame headlessly (DRM render node, no
 * display/DRM master needed -- same setup as tools/bench_render.c)
 * to a PPM screenshot, so a background-color rendering bug can be
 * eyeballed without needing to be physically at the console.
 *
 * Usage: headless_probe <out.ppm> <viewport_w> <viewport_h> <font_size> [settle_ms] [font_family] [font_variant] -- <cmd> [args...]
 *
 * cols/rows are DERIVED from viewport_w/viewport_h and the real cell
 * metrics for the given font (same order of operations as
 * core/main.c: cell size first, then cols=width/cell_w, rows=
 * height/cell_h) -- not passed directly -- so this can be pointed at
 * the exact real display size (e.g. 1920x1080) and font config a live
 * ghostcon-core session uses, instead of an arbitrary grid whose
 * cols/rows a TUI's own layout math may treat very differently.
 */

#define _DEFAULT_SOURCE

#include "ghostcon/core/egl.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ATLAS_DIM 1024
#define DRM_RENDER_NODE "/dev/dri/renderD128"

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out.ppm> [cols] [rows] [font_size] [settle_ms] -- <cmd> [args...]\n", argv[0]);
        return 1;
    }
    const char *out_path = argv[1];
    const char *raw_path = getenv("HEADLESS_PROBE_RAW_DUMP");
    FILE *raw_f = raw_path ? fopen(raw_path, "wb") : NULL;
    uint32_t target_vw = argc > 2 ? (uint32_t)atoi(argv[2]) : 1920;
    uint32_t target_vh = argc > 3 ? (uint32_t)atoi(argv[3]) : 1080;
    int font_size = argc > 4 ? atoi(argv[4]) : 20;
    int settle_ms = argc > 5 ? atoi(argv[5]) : 1500;
    const char *font_family = argc > 6 ? argv[6] : NULL;
    const char *font_variant = argc > 7 ? argv[7] : NULL;

    int cmd_idx = -1;
    for (int i = 8; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { cmd_idx = i + 1; break; }
    }
    if (cmd_idx < 0 || cmd_idx >= argc) {
        fprintf(stderr, "missing -- <cmd>\n");
        return 1;
    }

    ghostcon_atlas_t *atlas = ghostcon_atlas_create(font_family, font_variant, NULL, NULL, font_size, ATLAS_DIM);
    if (!atlas) { fprintf(stderr, "FAIL: atlas_create\n"); return 1; }
    int cell_w, cell_h;
    ghostcon_atlas_cell_size(atlas, &cell_w, &cell_h);
    uint16_t cols = (uint16_t)(target_vw / (uint32_t)cell_w);
    uint16_t rows = (uint16_t)(target_vh / (uint32_t)cell_h);
    uint32_t vw = (uint32_t)(cell_w * cols);
    uint32_t vh = (uint32_t)(cell_h * rows);
    fprintf(stderr, "cell %dx%d -> grid %ux%u (viewport %ux%u)\n", cell_w, cell_h, cols, rows, vw, vh);

    bool start_small = getenv("HEADLESS_PROBE_RESIZE_AFTER") != NULL;
    struct winsize ws = start_small
        ? (struct winsize){ .ws_row = 24, .ws_col = 80 }
        : (struct winsize){ .ws_row = rows, .ws_col = cols };
    int master_fd;
    pid_t child = forkpty(&master_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return 1;
    }
    if (child == 0) {
        const char *term = getenv("HEADLESS_PROBE_TERM");
        setenv("TERM", term ? term : "xterm-256color", 1);
        execvp(argv[cmd_idx], &argv[cmd_idx]);
        perror("execvp");
        _exit(127);
    }

    /* Drain the pty into the term model until the child's initial
       draw settles (no output for a bit) or settle_ms total elapses. */
    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, 0)) {
        fprintf(stderr, "FAIL: ghostcon_term_init\n");
        return 1;
    }

    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    if (start_small) {
        /* Give the child a moment to start up at the placeholder size,
           then deliver a REAL size change (not a same-size no-op) --
           this is what a real windowed terminal typically does (pty
           created at a default/placeholder size, then resized once
           the actual window geometry is known), and is what
           HEADLESS_PROBE_WINCH's no-op SIGWINCH test didn't cover. */
        usleep(150000);
        struct winsize real_ws = { .ws_row = rows, .ws_col = cols };
        ioctl(master_fd, TIOCSWINSZ, &real_ws);
    }

    double deadline_ms = settle_ms;
    struct pollfd pfd = { .fd = master_fd, .events = POLLIN };
    uint8_t buf[65536];
    for (;;) {
        int r = poll(&pfd, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) {
            deadline_ms -= 200;
            if (deadline_ms <= 0) break;
            continue;
        }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (raw_f) fwrite(buf, 1, (size_t)n, raw_f);
            ghostcon_term_feed(&term, buf, (size_t)n);
        }
    }

    if (getenv("HEADLESS_PROBE_WINCH")) {
        /* Real windowed terminals commonly deliver a SIGWINCH shortly
           after a program starts (window creation/focus) even when
           the size doesn't actually change; this synthetic pty never
           does. Test whether that's what triggers a fuller repaint. */
        kill(child, SIGWINCH);
        double extra_ms = 2000;
        for (;;) {
            int r = poll(&pfd, 1, 200);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) { extra_ms -= 200; if (extra_ms <= 0) break; continue; }
            if (pfd.revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(master_fd, buf, sizeof(buf));
                if (n <= 0) break;
                if (raw_f) fwrite(buf, 1, (size_t)n, raw_f);
                ghostcon_term_feed(&term, buf, (size_t)n);
            }
        }
    }

    /* Force a full repaint regardless of what's already marked dirty. */
    for (uint16_t y = 0; y < rows; y++)
        ghostcon_screen_mark_dirty(&term.screen, y);

    ghostcon_egl_t egl;
    if (!ghostcon_egl_init(&egl, DRM_RENDER_NODE, vw, vh)) {
        fprintf(stderr, "FAIL: egl_init on %s\n", DRM_RENDER_NODE);
        return 1;
    }
    if (!ghostcon_egl_make_current(&egl)) {
        fprintf(stderr, "FAIL: egl_make_current\n");
        return 1;
    }
    ghostcon_gles_t *gles = ghostcon_gles_create(vw, vh, false);
    if (!gles) { fprintf(stderr, "FAIL: gles_create\n"); return 1; }

    GhosttyColorRgb bg_rgb = term.screen.palette.bg_default;
    ghostcon_gles_begin(gles, true,
                         (float)bg_rgb.r / 255.0f,
                         (float)bg_rgb.g / 255.0f,
                         (float)bg_rgb.b / 255.0f);
    ghostcon_machine_render_dirty(&term.screen, atlas, gles, cell_w, cell_h);
    ghostcon_machine_render_cursor(&term.screen, gles, cell_w, cell_h);
    ghostcon_gles_sync_atlas(gles, atlas, false);
    ghostcon_gles_end(gles);

    if (!ghostcon_gles_screenshot_ppm(gles, out_path)) {
        fprintf(stderr, "FAIL: screenshot_ppm\n");
        return 1;
    }
    fprintf(stderr, "wrote %s (%ux%u)\n", out_path, vw, vh);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (raw_f) fclose(raw_f);
    return 0;
}
