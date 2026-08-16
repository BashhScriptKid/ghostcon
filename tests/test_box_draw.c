/*
 * test_box_draw — pure-CPU unit test for the procedural box-drawing/
 * block-element/sextant rasterizer (render/box_draw.c). No GPU/DRM
 * dependency, unlike test_render.c: this only exercises the coverage
 * bitmap generation directly.
 */

#include "ghostcon/render/box_draw.h"

#include <stdio.h>

#define CELL_W 10
#define CELL_H 20

static int failures = 0;

static void
check(const char *name, bool cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static bool
rect_all(const uint8_t *alpha, int x0, int y0, int x1, int y1, uint8_t expect)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (alpha[y * CELL_W + x] != expect)
                return false;
    return true;
}

int
main(void)
{
    uint8_t alpha[CELL_W * CELL_H];

    /* Unhandled codepoint (ordinary letter) must fall through. */
    check("unhandled codepoint returns false",
          !ghostcon_box_draw_render('A', CELL_W, CELL_H, alpha));

    /* U+2588 FULL BLOCK -- entire cell filled. */
    check("full block handled",
          ghostcon_box_draw_render(0x2588, CELL_W, CELL_H, alpha));
    check("full block is fully covered",
          rect_all(alpha, 0, 0, CELL_W, CELL_H, 255));

    /* U+2580 UPPER HALF BLOCK -- top half filled, bottom half empty. */
    check("upper half block handled",
          ghostcon_box_draw_render(0x2580, CELL_W, CELL_H, alpha));
    check("upper half: top filled", rect_all(alpha, 0, 0, CELL_W, CELL_H / 2, 255));
    check("upper half: bottom empty", rect_all(alpha, 0, CELL_H / 2, CELL_W, CELL_H, 0));

    /* U+2596 QUADRANT LOWER LEFT -- only the LL quadrant filled. */
    check("quadrant lower-left handled",
          ghostcon_box_draw_render(0x2596, CELL_W, CELL_H, alpha));
    check("quadrant LL: UL empty", rect_all(alpha, 0, 0, CELL_W / 2, CELL_H / 2, 0));
    check("quadrant LL: UR empty", rect_all(alpha, CELL_W / 2, 0, CELL_W, CELL_H / 2, 0));
    check("quadrant LL: LL filled", rect_all(alpha, 0, CELL_H / 2, CELL_W / 2, CELL_H, 255));
    check("quadrant LL: LR empty", rect_all(alpha, CELL_W / 2, CELL_H / 2, CELL_W, CELL_H, 0));

    /* U+1FB00 BLOCK SEXTANT-1 -- top-left cell of the 2x3 grid only
       (idx=0, mask = 0 + 0/20 + 1 = 1 = bit0 = tl). */
    check("sextant-1 handled",
          ghostcon_box_draw_render(0x1fb00, CELL_W, CELL_H, alpha));
    check("sextant-1: tl filled", rect_all(alpha, 0, 0, CELL_W / 2, CELL_H / 3, 255));
    check("sextant-1: tr empty", rect_all(alpha, CELL_W / 2, 0, CELL_W, CELL_H / 3, 0));
    check("sextant-1: rest empty", rect_all(alpha, 0, CELL_H / 3, CELL_W, CELL_H, 0));

    /* U+253C LIGHT CROSS -- all four arms present (spot-check: center
       column and center row both have coverage). */
    check("light cross handled",
          ghostcon_box_draw_render(0x253c, CELL_W, CELL_H, alpha));
    check("light cross: center pixel set", alpha[(CELL_H / 2) * CELL_W + CELL_W / 2] == 255);
    check("light cross: top-left corner empty", alpha[0] == 0);
    check("light cross: top-right corner empty", alpha[CELL_W - 1] == 0);

    /* U+256D ARC DOWN AND RIGHT (╭) -- top-left quadrant relative to
       center must stay empty (that's the whole point of rounding: the
       far corner is cut off), while the straight arms reach the right
       and bottom edges. */
    check("rounded corner (down+right) handled",
          ghostcon_box_draw_render(0x256d, CELL_W, CELL_H, alpha));
    check("rounded corner: top-left quadrant empty",
          rect_all(alpha, 0, 0, CELL_W / 2, CELL_H / 2, 0));
    check("rounded corner: right edge midline filled",
          alpha[(CELL_H / 2) * CELL_W + (CELL_W - 1)] == 255);
    check("rounded corner: bottom edge midline filled",
          alpha[(CELL_H - 1) * CELL_W + CELL_W / 2] == 255);

    /* U+2579 BOX DRAWINGS HEAVY UP (╹) -- single arm reaching the top
       edge, no down arm reaching the bottom edge. Found live: a TUI
       border used this as a terminator cap; it fell through to the
       font atlas and showed as a short mark disconnected from the
       solid line above it. */
    check("heavy-up terminator handled",
          ghostcon_box_draw_render(0x2579, CELL_W, CELL_H, alpha));
    check("heavy-up: top edge filled", alpha[CELL_W / 2] == 255);
    check("heavy-up: bottom edge empty", alpha[(CELL_H - 1) * CELL_W + CELL_W / 2] == 0);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_box_draw: all checks passed\n");
    return 0;
}
