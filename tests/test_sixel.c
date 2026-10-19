/* test_sixel
 *
 * Unit-tests ghostcon_sixel_decode() (term/sixel.c) directly, without a screen or
 * terminal: builds DCS-style sixel bodies, decodes them, and inspects the
 * resulting placement's dimensions, pixels, and reported cursor move.
 *
 * The decode path had no automated coverage before (only
 * tools/image_protocol_bench.c drives it end to end), so this pins the
 * measure/paint split introduced by the state-machine refactor.
 */

#include "ghostcon/term/sixel.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static const uint8_t *pixel_at(const ghostcon_sixel_placement_t *pl, int32_t x, int32_t y)
{
    return pl->pixels + ((size_t)y * (size_t)pl->width + (size_t)x) * 4;
}

static bool decode(ghostcon_sixel_state_t *st, const char *body, const int32_t *params, size_t nparams, ghostcon_sixel_cursor_move_t *mv)
{
    ghostcon_sixel_decode(st, params, nparams, body, strlen(body),
                          2, 3, 10, 20, mv);
    return st->placements[0].in_use;
}

int main(void)
{
    ghostcon_sixel_state_t st;
    ghostcon_sixel_cursor_move_t mv;

    /* --- single red column --- */
    ghostcon_sixel_state_init(&st);
    bool got = decode(&st, "#0;2;100;0;0~", NULL, 0, &mv);
    CHECK(got, "one red column: a placement is created");
    const ghostcon_sixel_placement_t *pl = &st.placements[0];
    CHECK(pl->width == 1 && pl->height == 6,
          "one red column: 1x6 raster from the sixel scan");
    bool all_red = true;
    for (int y = 0; y < 6; y++)
    {
        const uint8_t *px = pixel_at(pl, 0, y);
        if (px[0] != 255 || px[1] != 0 || px[2] != 0 || px[3] != 255)
            all_red = false;
    }
    CHECK(all_red, "one red column: every pixel is opaque RGB(255,0,0)");
    CHECK(mv.moved && mv.rows == 1 && mv.col == 3,
          "cursor move: 1 row down, anchor_col + 1 col");
    ghostcon_sixel_state_deinit(&st);

    /* --- repeat introducer paints N columns --- */
    ghostcon_sixel_state_init(&st);
    got = decode(&st, "#0;2;100;0;0!3~", NULL, 0, &mv);
    pl = &st.placements[0];
    CHECK(got && pl->width == 3 && pl->height == 6, "!3 repeat: 3x6 raster");
    bool rep_red = true;
    for (int x = 0; x < 3; x++)
    {
        if (pixel_at(pl, x, 0)[0] != 255)
            rep_red = false;
    }
    CHECK(rep_red, "!3 repeat: all three columns painted");
    ghostcon_sixel_state_deinit(&st);

    /* --- raster attribute declares the canvas size --- */
    ghostcon_sixel_state_init(&st);
    got = decode(&st, "\"1;1;4;8#0;2;100;0;0~", NULL, 0, &mv);
    pl = &st.placements[0];
    CHECK(got && pl->width == 4 && pl->height == 8,
          "raster attribute: declared 4x8 canvas is honored");
    ghostcon_sixel_state_deinit(&st);

    /* --- P2 background select: opaque vs transparent untouched pixels ---
     * '?' carries 0 bits, so it advances x without painting anything.
     */
    ghostcon_sixel_state_init(&st);
    int32_t opaque[2] = {0, 0};
    got = decode(&st, "?", opaque, 2, &mv);
    pl = &st.placements[0];
    const uint8_t *bg = pixel_at(pl, 0, 0);
    CHECK(got && bg[0] == 0 && bg[1] == 0 && bg[2] == 0 && bg[3] == 255,
          "P2=0: untouched pixels are opaque black");
    ghostcon_sixel_state_deinit(&st);

    ghostcon_sixel_state_init(&st);
    int32_t transparent[2] = {0, 1};
    got = decode(&st, "?", transparent, 2, &mv);
    pl = &st.placements[0];
    bg = pixel_at(pl, 0, 0);
    CHECK(got && bg[3] == 0,
          "P2=1: untouched pixels stay transparent (alpha 0)");
    ghostcon_sixel_state_deinit(&st);

    /* --- HLS color introducer (hue 0, lightness 100, saturation 0 = white) --- */
    ghostcon_sixel_state_init(&st);
    got = decode(&st, "#1;1;0;100;0~", NULL, 0, &mv);
    pl = &st.placements[0];
    const uint8_t *w = pixel_at(pl, 0, 0);
    CHECK(got && w[0] == 255 && w[1] == 255 && w[2] == 255,
          "HLS introducer: L=100 S=0 yields white");
    ghostcon_sixel_state_deinit(&st);

    /* --- empty body: nothing decodable, no placement, no move --- */
    ghostcon_sixel_state_init(&st);
    ghostcon_sixel_decode(&st, NULL, 0, "", 0, 0, 0, 10, 20, &mv);
    CHECK(!st.placements[0].in_use, "empty body: no placement is created");
    CHECK(!mv.moved, "empty body: cursor does not move");
    ghostcon_sixel_state_deinit(&st);

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
