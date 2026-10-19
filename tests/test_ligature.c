/* test_ligature -- opt-in programming-ligature detection (see
 * render/ligature.h).
 *
 * No GL/EGL needed: ghostcon_ligature_scan_row() only touches
 * FreeType/HarfBuzz through the atlas, same as any other glyph lookup.
 *
 * Feeds known operator sequences through a real font (JetBrainsMono Nerd
 * Font Mono -- the font configured for the live deploy, see CLAUDE.md) and
 * asserts the scanner reports collapsed runs for the ones that font is
 * known to substitute a single ligature glyph for, and reports nothing for
 * plain text / isolated operators that should never collapse.
 */

#include "ghostcon/render/atlas.h"
#include "ghostcon/render/ligature.h"
#include "ghostcon/term/term.h"

#include <stdio.h>
#include <string.h>

#define FONT_FAMILY "JetBrainsMono Nerd Font Mono"
#define FONT_SIZE   16
#define ATLAS_DIM   512
#define COLS        40
#define ROWS        2

static int failures = 0;

static int scan_line(ghostcon_atlas_t *atlas, const char *text,
                     ghostcon_ligature_glyph_t *out_glyphs, int max_glyphs)
{
    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, COLS, ROWS, 0))
    {
        fprintf(stderr, "FAIL: ghostcon_term_init\n");
        failures++;
        return -1;
    }
    ghostcon_term_feed(&term, (const uint8_t *)text, strlen(text));

    ghostcon_row_t *row = ghostcon_screen_row(&term.screen, 0);
    int n = ghostcon_ligature_scan_row(row, atlas, out_glyphs, max_glyphs);

    ghostcon_term_deinit(&term);
    return n;
}

int main(void)
{
    ghostcon_atlas_t *atlas = ghostcon_atlas_create(FONT_FAMILY, NULL, NULL, NULL,
                                                      FONT_SIZE, ATLAS_DIM);
    if (!atlas)
    {
        fprintf(stderr, "FAIL: ghostcon_atlas_create(%s)\n", FONT_FAMILY);
        return 1;
    }
    printf("PASS: atlas created with %s\n", FONT_FAMILY);

    /* Known JetBrains Mono ligature sequences (its own font spec lists
     * arrows, comparison chains, etc. under calt).
     *
     * If none of these collapse, either the installed build lacks calt (a
     * real, useful finding) or ghostcon_atlas_shape_run() isn't reaching
     * HarfBuzz --either way this test should fail loudly rather than
     * silently pass on zero runs found.
     */
    static const char *const positive_cases[] = {
        "x -> y",
        "a == b",
        "a != b",
        "a <= b",
        "a >= b",
    };
    int any_positive = 0;
    for (size_t i = 0; i < sizeof(positive_cases) / sizeof(positive_cases[0]); i++)
    {
        ghostcon_ligature_glyph_t glyphs[GHOSTCON_LIGATURE_MAX_RUN_LEN];
        int n = scan_line(atlas, positive_cases[i], glyphs, GHOSTCON_LIGATURE_MAX_RUN_LEN);
        if (n < 0)
            continue; /* scan_line already recorded the failure */
        if (n > 0)
        {
            printf("PASS: \"%s\" -> %d substituted glyph(s), first at x=%u (glyph %u)\n",
                   positive_cases[i], n, glyphs[0].x, glyphs[0].glyph_index);
            any_positive = 1;
        }
        else
        {
            printf("NOTE: \"%s\" -> no ligature substitution detected\n", positive_cases[i]);
        }
    }
    if (!any_positive)
    {
        fprintf(stderr,
                "FAIL: no positive case produced a ligature run -- either %s has no calt "
                "ligatures for these sequences, or shaping isn't reaching HarfBuzz\n",
                FONT_FAMILY);
        failures++;
    }

    /* Negative cases: plain text and an isolated single operator must never
     * be reported as a ligature run.
     */
    static const char *const negative_cases[] = {
        "hello world",
        "a + b",
    };
    for (size_t i = 0; i < sizeof(negative_cases) / sizeof(negative_cases[0]); i++)
    {
        ghostcon_ligature_glyph_t glyphs[GHOSTCON_LIGATURE_MAX_RUN_LEN];
        int n = scan_line(atlas, negative_cases[i], glyphs, GHOSTCON_LIGATURE_MAX_RUN_LEN);
        if (n < 0)
            continue;
        if (n != 0)
        {
            fprintf(stderr, "FAIL: \"%s\" unexpectedly produced %d substituted glyph(s)\n",
                    negative_cases[i], n);
            failures++;
        }
        else
        {
            printf("PASS: \"%s\" -> no ligature run (expected)\n", negative_cases[i]);
        }
    }

    ghostcon_atlas_destroy(atlas);

    if (failures > 0)
    {
        fprintf(stderr, "FAILED: %d failure(s)\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
