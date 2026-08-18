/*
 * bench_parse -- measures VT-parsing throughput (ghostcon_term_feed(),
 * i.e. term/stream.c + the vendored libghostty-vt state machine),
 * isolated from rendering, pty I/O, and everything else.
 *
 * Motivation: bench_render showed the GLES render pipeline itself is
 * nowhere near the bottleneck for "fast scroll looks like a slideshow,
 * skipped frames" (>600fps worst case, headless). The likely remaining
 * culprit: if a fast-scrolling TUI produces escape-sequence output
 * faster than this can parse it, ghostcon-core's main loop only
 * renders whatever state happens to exist once the backlog is
 * drained -- which looks exactly like dropped/skipped frames, not
 * smooth-but-slow motion, since every intermediate scroll position in
 * between never gets rendered at all.
 *
 * Feeds synthetic output shaped like a real full-screen TUI redraw
 * (cursor-home + SGR-colored lines filling the whole grid, repeated --
 * same shape htop/btop/etc. produce on every refresh), in pty-read-
 * sized chunks (4096 bytes, matching ptyserv/pty_child.c's own read
 * buffer size) rather than one giant feed call, since that's the real
 * call-pattern shape. Reports MB/s and "screens/sec" (how many
 * full-screen-worth-of-redraw-bytes can be parsed per second) so it's
 * directly comparable against how fast a real TUI can actually produce
 * output.
 *
 * Usage: bench_parse [cols] [rows] [screens]
 *   Defaults: 128x32, 2000 synthetic full-screen redraws.
 */

#define _DEFAULT_SOURCE /* clock_gettime()/CLOCK_MONOTONIC under -std=c11 without this */

#include "ghostcon/term/term.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define READ_CHUNK 4096 /* matches ptyserv/pty_child.c's own buf[4096] */

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Builds one full-screen redraw: CSI H (cursor home, what a TUI does
   before repainting) then `rows` lines of SGR-colored, varied-glyph
   content filling every column -- same content-generation approach as
   bench_render.c's fill_scrollback(), for the same reason (real color/
   glyph variety, not a degenerate single-repeated-byte case). Returns
   bytes written. */
static size_t
build_one_screen(char *out, size_t out_cap, uint16_t cols, uint16_t rows, int screen_idx)
{
    size_t off = 0;
    off += (size_t)snprintf(out + off, out_cap - off, "\x1b[H");
    for (uint16_t r = 0; r < rows; r++) {
        int color = 31 + ((screen_idx + r) % 6);
        off += (size_t)snprintf(out + off, out_cap - off, "\x1b[%dm", color);
        for (uint16_t c = 0; c < cols; c++)
            out[off++] = (char)('!' + ((screen_idx * 5 + r * 7 + c * 13) % 94));
        off += (size_t)snprintf(out + off, out_cap - off, "\x1b[0m\r\n");
    }
    return off;
}

int
main(int argc, char **argv)
{
    uint16_t cols = argc > 1 ? (uint16_t)atoi(argv[1]) : 128;
    uint16_t rows = argc > 2 ? (uint16_t)atoi(argv[2]) : 32;
    int screens = argc > 3 ? atoi(argv[3]) : 2000;

    if (cols == 0 || rows == 0 || screens <= 0) {
        fprintf(stderr, "usage: %s [cols] [rows] [screens]\n", argv[0]);
        return 1;
    }

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, 0)) {
        fprintf(stderr, "FAIL: ghostcon_term_init\n");
        return 1;
    }

    size_t screen_cap = (size_t)cols * rows * 12 + 256; /* generous -- SGR + glyph per cell */
    char *screen_buf = malloc(screen_cap);
    if (!screen_buf) {
        fprintf(stderr, "FAIL: malloc\n");
        return 1;
    }

    /* Pre-generate all screens' bytes up front so the timed loop below
       measures ONLY parse throughput, not this synthesis work. */
    char **screen_data = malloc((size_t)screens * sizeof(char *));
    size_t *screen_len = malloc((size_t)screens * sizeof(size_t));
    size_t total_bytes = 0;
    for (int i = 0; i < screens; i++) {
        size_t n = build_one_screen(screen_buf, screen_cap, cols, rows, i);
        screen_data[i] = malloc(n);
        memcpy(screen_data[i], screen_buf, n);
        screen_len[i] = n;
        total_bytes += n;
    }
    free(screen_buf);

    printf("bench_parse: %ux%u cells, %d synthetic full-screen redraws, %zu bytes total\n",
           cols, rows, screens, total_bytes);

    /* Feed everything through in READ_CHUNK-sized pieces, spanning
       screen boundaries arbitrarily (a real pty read has no idea where
       one TUI redraw ends and the next begins either) -- flatten into
       one contiguous buffer first so chunking is exact. */
    char *all = malloc(total_bytes);
    size_t off = 0;
    for (int i = 0; i < screens; i++) {
        memcpy(all + off, screen_data[i], screen_len[i]);
        off += screen_len[i];
        free(screen_data[i]);
    }
    free(screen_data);
    free(screen_len);

    double t0 = now_ms();
    size_t fed = 0;
    while (fed < total_bytes) {
        size_t n = total_bytes - fed;
        if (n > READ_CHUNK) n = READ_CHUNK;
        ghostcon_term_feed(&term, (const uint8_t *)(all + fed), n);
        fed += n;
    }
    double elapsed_ms = now_ms() - t0;

    double mb = (double)total_bytes / (1024.0 * 1024.0);
    double mb_per_s = mb / (elapsed_ms / 1000.0);
    double screens_per_s = (double)screens / (elapsed_ms / 1000.0);
    double bytes_per_screen = (double)total_bytes / screens;

    printf("\n--- results ---\n");
    printf("elapsed:        %8.2f ms\n", elapsed_ms);
    printf("throughput:     %8.2f MB/s\n", mb_per_s);
    printf("screens/sec:    %8.1f  (a full-screen redraw = %.0f bytes at this size)\n",
           screens_per_s, bytes_per_screen);
    printf("us per screen:  %8.2f\n", (elapsed_ms * 1000.0) / screens);
    printf("\nFor reference: a fast-scrolling TUI producing a full-screen\n"
           "redraw every 16.7ms (60fps) needs >= 60 screens/sec here to\n"
           "keep up without falling behind and skipping frames.\n");

    free(all);
    ghostcon_term_deinit(&term);
    return 0;
}
