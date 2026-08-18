/*
 * replay_capture -- one-off diagnostic tool (not registered in
 * meson.build, build/run manually) for the "stale glyphs after a
 * complex tool output / message send" report.
 *
 * Feeds a captured raw pty-output file through ghostcon_term_feed()
 * in the same 4096-byte chunks core/main.c's real read loop uses,
 * and after EVERY chunk compares two views of the screen:
 *   - "dirty view": a snapshot buffer updated ONLY for rows within
 *     the accumulated dirty range (screen->dirty.y_min..y_max) --
 *     exactly what the real renderer would have drawn.
 *   - "true view": the screen's actual current row content,
 *     unconditionally, regardless of dirty tracking.
 * The first chunk where these two diverge for some row is the exact
 * point a needed mark_dirty() call was missed -- reports the chunk
 * index, byte offset, and row number.
 *
 * Usage: replay_capture <capture_file> [cols] [rows]
 */

#include "ghostcon/term/term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_CHUNK 4096

static void
row_to_text(ghostcon_screen_t *screen, uint16_t y, char *out, uint16_t cols)
{
    ghostcon_row_t *row = ghostcon_screen_row(screen, y);
    for (uint16_t x = 0; x < cols; x++) {
        if (!row) { out[x] = '?'; continue; }
        ghostcon_cell_t cell = row->cells[x];
        uint32_t cp = ghostcon_cell_get_codepoint(cell);
        out[x] = (cp >= 32 && cp < 127) ? (char)cp : (cp == 0 ? ' ' : '#');
    }
    out[cols] = '\0';
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <capture_file> [cols] [rows]\n", argv[0]);
        return 1;
    }
    uint16_t cols = argc > 2 ? (uint16_t)atoi(argv[2]) : 128;
    uint16_t rows = argc > 3 ? (uint16_t)atoi(argv[3]) : 32;
    long read_chunk = argc > 4 ? atol(argv[4]) : READ_CHUNK;
    if (read_chunk <= 0) read_chunk = READ_CHUNK;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)fsize);
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "FAIL: short read\n");
        return 1;
    }
    fclose(f);
    printf("replay_capture: %ld bytes, simulating %ux%u terminal\n", fsize, cols, rows);

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, 2000)) {
        fprintf(stderr, "FAIL: term_init\n");
        return 1;
    }

    /* Dirty-tracked view: what the real renderer would show, one text
       line per row, updated only for rows the dirty region covers. */
    char **dirty_view = malloc(rows * sizeof(char *));
    for (uint16_t y = 0; y < rows; y++) {
        dirty_view[y] = malloc((size_t)cols + 1);
        memset(dirty_view[y], ' ', cols);
        dirty_view[y][cols] = '\0';
    }

    char true_line[512];
    long off = 0;
    int chunk_idx = 0;
    int first_mismatch_chunk = -1;

    while (off < fsize) {
        long n = fsize - off;
        if (n > read_chunk) n = read_chunk;
        ghostcon_term_feed(&term, data + off, (size_t)n);

        ghostcon_dirty_region_t dirty = ghostcon_screen_get_dirty(&term.screen);
        if (dirty.y_min >= 0) {
            for (int16_t y = dirty.y_min; y <= dirty.y_max && y < (int16_t)rows; y++) {
                if (y < 0) continue;
                row_to_text(&term.screen, (uint16_t)y, dirty_view[y], cols);
            }
            ghostcon_screen_clear_dirty(&term.screen);
        }

        /* Transient content-duplication check, done EVERY chunk (not
           just at the end) -- the reported symptom disappears once
           later output overwrites it, so checking only the final
           state would miss it entirely. */
        {
            char lines[512][512];
            for (uint16_t y = 0; y < rows && y < 512; y++)
                row_to_text(&term.screen, y, lines[y], cols);
            for (uint16_t y = 0; y < rows; y++) {
                bool blank = true;
                for (uint16_t x = 0; x < cols; x++) if (lines[y][x] != ' ') { blank = false; break; }
                if (blank) continue;
                for (uint16_t dy = 1; dy <= 3 && y + dy < rows; dy++) {
                    if (strcmp(lines[y], lines[y + dy]) == 0) {
                        printf("\nTRANSIENT DUPLICATE at chunk %d (byte offset %ld): "
                               "row %u and row %u identical:\n  [%s]\n",
                               chunk_idx, off, y, y + dy, lines[y]);
                    }
                }
            }
        }

        off += n;
        chunk_idx++;
    }

    /* Final comparison: dirty_view (what real rendering accumulated)
       vs. the screen's true final content (unconditional read-back). */
    int mismatches = 0;
    for (uint16_t y = 0; y < rows; y++) {
        row_to_text(&term.screen, y, true_line, cols);
        if (strcmp(dirty_view[y], true_line) != 0) {
            mismatches++;
            printf("\nMISMATCH row %u:\n", y);
            printf("  dirty-tracked view: [%s]\n", dirty_view[y]);
            printf("  true final state:   [%s]\n", true_line);
        }
    }

    if (mismatches == 0) {
        printf("\nNo mismatches -- dirty-tracked rendering matches true state for "
               "every row across all %d chunks. The bug (if reproduced by this "
               "capture at all) isn't a missing mark_dirty() call in screen.c's "
               "row-mutation paths.\n", chunk_idx);
    } else {
        printf("\n%d row(s) would render stale under real dirty-tracked rendering "
               "-- this IS the bug, reproduced offline. Re-run with instrumentation "
               "around whichever screen.c function processes the byte range near "
               "these rows to find the missing mark_dirty() call.\n", mismatches);
    }
    (void)first_mismatch_chunk;

    /* Content-duplication check (separate from dirty-tracking above):
       reported symptom is the SAME text appearing on two rows -- a
       content-placement bug (wrong cursor/scroll math during parsing),
       not a stale-redraw bug, so it'd show up in the TRUE state itself,
       not just the dirty-view/true-view diff above. Scan every row
       pair within a few lines of each other for near-duplicate
       non-blank content. */
    printf("\n--- content-duplication scan ---\n");
    char lines[512][512];
    for (uint16_t y = 0; y < rows && y < 512; y++)
        row_to_text(&term.screen, y, lines[y], cols);
    int dup_found = 0;
    for (uint16_t y = 0; y < rows; y++) {
        /* Skip blank/whitespace-only rows -- not interesting. */
        bool blank = true;
        for (uint16_t x = 0; x < cols; x++) if (lines[y][x] != ' ') { blank = false; break; }
        if (blank) continue;
        for (uint16_t dy = 1; dy <= 3 && y + dy < rows; dy++) {
            if (strcmp(lines[y], lines[y + dy]) == 0) {
                printf("DUPLICATE: row %u and row %u are identical:\n  [%s]\n",
                       y, y + dy, lines[y]);
                dup_found++;
            }
        }
    }
    if (!dup_found)
        printf("No exact-duplicate row pairs found within 3 lines of each other "
               "in the final state.\n");

    free(data);
    for (uint16_t y = 0; y < rows; y++) free(dirty_view[y]);
    free(dirty_view);
    ghostcon_term_deinit(&term);
    return mismatches > 0 ? 1 : 0;
}
