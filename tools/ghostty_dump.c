/*
 * ghostty_dump — reference terminal-state dumper.
 *
 * Feeds a byte stream into a real Ghostty terminal core (libghostty-vt)
 * and dumps the resulting screen state in a canonical, diffable format.
 *
 * Used by the Phase 0 validation harness to compare ghostcon against the
 * reference implementation. See run_compare.sh.
 *
 * Canonical dump format:
 *   cols=<n>
 *   rows=<n>
 *   screen=primary|alt
 *   cursor_x=<x>
 *   cursor_y=<y>
 *   pending_wrap=<0|1>
 *   row<y>wrap=<0|1>          (one per visible row)
 *   cell <x> <y> cp=0x%06X wide=<0-3> prot=<0|1> styled=<0|1>
 *                             (one per non-empty cell)
 *
 * A cell is considered "empty" when cp==0, wide==0, prot==0, styled==0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <ghostty/vt.h>

static void
dump_cell(GhosttyTerminal term, uint16_t x, uint16_t y)
{
    GhosttyPoint pt;
    pt.tag = GHOSTTY_POINT_TAG_ACTIVE;
    pt.value.coordinate.x = x;
    pt.value.coordinate.y = y;

    GhosttyGridRef ref = { .size = sizeof(ref) };
    if (ghostty_terminal_grid_ref(term, pt, &ref) != GHOSTTY_SUCCESS)
        return;

    GhosttyCell cell = 0;
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS)
        return;

    uint32_t cp = 0;
    uint32_t wide = 0;
    uint32_t prot = 0;
    uint32_t styled = 0;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_CODEPOINT, &cp);
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_PROTECTED, &prot);
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_STYLING, &styled);

    if (cp == 0 && wide == 0 && prot == 0 && styled == 0)
        return;

    printf("cell %u %u cp=0x%06X wide=%u prot=%u styled=%u\n",
           x, y, cp, wide, prot, styled);
}

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

    GhosttyTerminal term = NULL;
    if (ghostty_terminal_new(NULL, &term, cols, rows) != GHOSTTY_SUCCESS) {
        fprintf(stderr, "ghostty_terminal_new failed\n");
        return 1;
    }

    /* Feed in deterministic chunks so split-sequence handling is exercised
       identically on both sides. */
    const size_t chunk = 256;
    for (long off = 0; off < len; off += (long)chunk) {
        size_t n = (size_t)len - (size_t)off;
        if (n > chunk) n = chunk;
        ghostty_terminal_vt_write(term, buf + off, n);
    }

    uint16_t out_cols = 0, out_rows = 0, cx = 0, cy = 0;
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    uint32_t pending = 0;
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_COLS, &out_cols);
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_ROWS, &out_rows);
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_CURSOR_PENDING_WRAP, &pending);
    ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);

    printf("cols=%u\n", out_cols);
    printf("rows=%u\n", out_rows);
    printf("screen=%s\n", screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE ? "alt" : "primary");
    printf("cursor_x=%u\n", cx);
    printf("cursor_y=%u\n", cy);
    printf("pending_wrap=%u\n", pending);

    for (uint16_t y = 0; y < out_rows; y++) {
        GhosttyPoint pt;
        pt.tag = GHOSTTY_POINT_TAG_ACTIVE;
        pt.value.coordinate.x = 0;
        pt.value.coordinate.y = y;

        GhosttyGridRef ref = { .size = sizeof(ref) };
        uint32_t wrap = 0;
        if (ghostty_terminal_grid_ref(term, pt, &ref) == GHOSTTY_SUCCESS) {
            GhosttyRow row = 0;
            if (ghostty_grid_ref_row(&ref, &row) == GHOSTTY_SUCCESS)
                ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &wrap);
        }
        printf("row%uwrap=%u\n", y, wrap);
    }

    for (uint16_t y = 0; y < out_rows; y++)
        for (uint16_t x = 0; x < out_cols; x++)
            dump_cell(term, x, y);

    ghostty_terminal_free(term);
    free(buf);
    return 0;
}
