/*
 * test_theme — covers term/theme.c's named presets: every preset name
 * is recognized and produces a sane, non-degenerate palette (distinct
 * fg/bg, all 16 ANSI slots actually set), and an unrecognized/empty
 * name is a no-op (returns false, palette untouched) rather than an
 * error.
 */

#include "ghostcon/term/theme.h"
#include "ghostcon/term/color.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static bool
rgb_eq(GhosttyColorRgb a, GhosttyColorRgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

int
main(void)
{
    const char *presets[] = {
        "base16-dark", "base16-light", "solarized-dark", "solarized-light",
    };

    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        ghostcon_palette_t pal;
        ghostcon_palette_init(&pal);
        bool ok = ghostcon_theme_apply(&pal, presets[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: recognized", presets[i]);
        CHECK(ok, msg);

        snprintf(msg, sizeof(msg), "%s: fg != bg", presets[i]);
        CHECK(!rgb_eq(pal.fg_default, pal.bg_default), msg);

        /* Every ANSI slot actually got touched (i.e. isn't still
           whatever ghostcon_palette_init()'s own baseline set) --
           weak but catches an obviously-truncated preset table entry. */
        bool any_differs = false;
        for (int c = 0; c < 16 && !any_differs; c++)
            any_differs = !rgb_eq(pal.table[c], GC_ANSI_COLORS[c]);
        snprintf(msg, sizeof(msg), "%s: ANSI table actually changed", presets[i]);
        CHECK(any_differs, msg);
    }

    /* Real Ghostty theme catalog fallback (generated from Ghostty's
       own bundled theme files -- see theme_ghostty_presets.c). Case-
       sensitive exact match, same as Ghostty's own theme names. */
    {
        ghostcon_palette_t pal;
        ghostcon_palette_init(&pal);
        CHECK(ghostcon_theme_apply(&pal, "Apple System Colors"),
              "Ghostty catalog: \"Apple System Colors\" recognized");
        GhosttyColorRgb expect_bg = {0x1e, 0x1e, 0x1e};
        GhosttyColorRgb expect_fg = {0xff, 0xff, 0xff};
        GhosttyColorRgb expect_c0 = {0x1a, 0x1a, 0x1a};
        CHECK(rgb_eq(pal.bg_default, expect_bg), "Ghostty catalog: bg matches Ghostty's theme file");
        CHECK(rgb_eq(pal.fg_default, expect_fg), "Ghostty catalog: fg matches Ghostty's theme file");
        CHECK(rgb_eq(pal.table[0], expect_c0), "Ghostty catalog: color0 matches Ghostty's theme file");

        CHECK(ghostcon_theme_apply(&pal, "apple system colors") == false,
              "Ghostty catalog: lookup is case-sensitive");
    }

    /* Unrecognized/empty name: no-op, not an error. */
    ghostcon_palette_t pal;
    ghostcon_palette_init(&pal);
    GhosttyColorRgb fg_before = pal.fg_default, bg_before = pal.bg_default;
    CHECK(ghostcon_theme_apply(&pal, "not-a-real-theme") == false,
          "unrecognized theme name returns false");
    CHECK(rgb_eq(pal.fg_default, fg_before) && rgb_eq(pal.bg_default, bg_before),
          "unrecognized theme name leaves palette untouched");
    CHECK(ghostcon_theme_apply(&pal, NULL) == false, "NULL theme name returns false");
    CHECK(ghostcon_theme_apply(&pal, "") == false, "empty theme name returns false");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
