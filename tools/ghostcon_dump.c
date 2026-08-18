/*
 * ghostcon_dump — ghostcon terminal-state dumper.
 *
 * Feeds a byte stream into ghostcon's terminal core and dumps the
 * resulting screen state in the canonical, diffable format shared with
 * tools/ghostty_dump.c (see that file for the format spec).
 *
 * Used by the Phase 0 validation harness to compare ghostcon against
 * the reference Ghostty implementation. See run_compare.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ghostcon/term/term.h"
#include "ghostcon/term/dump.h"

int
main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s COLS ROWS INPUT\n", argv[0]);
        return 2;
    }

    uint16_t cols = (uint16_t)strtoul(argv[1], NULL, 10);
    uint16_t rows = (uint16_t)strtoul(argv[2], NULL, 10);
    const char *path = argv[3];
    if (cols == 0 || rows == 0) {
        fprintf(stderr, "cols/rows must be > 0\n");
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len > 0 ? (size_t)len : 1);
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        perror("fread");
        return 2;
    }
    fclose(f);

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, 0)) {
        fprintf(stderr, "ghostcon_term_init failed\n");
        return 1;
    }

    /* Same deterministic chunking as the reference dumper. */
    const size_t chunk = 256;
    for (long off = 0; off < len; off += (long)chunk) {
        size_t n = (size_t)len - (size_t)off;
        if (n > chunk) n = chunk;
        ghostcon_term_feed(&term, buf + off, n);
    }

    ghostcon_screen_dump(stdout, &term.screen);

    ghostcon_term_deinit(&term);
    free(buf);
    return 0;
}
