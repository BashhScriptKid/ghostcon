#include "ghostcon/render/box_draw.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static void
fill_rect(uint8_t *alpha, int cell_w, int cell_h,
          int x0, int y0, int x1, int y1, uint8_t v)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cell_w) x1 = cell_w;
    if (y1 > cell_h) y1 = cell_h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            alpha[y * cell_w + x] = v;
}

/* ------------------------------------------------------------------ */
/* Block Elements, U+2580-259F                                         */
/* ------------------------------------------------------------------ */

static bool
render_block_element(uint32_t cp, int cell_w, int cell_h, uint8_t *alpha)
{
    if (cp == 0x2580) { /* upper half block */
        fill_rect(alpha, cell_w, cell_h, 0, 0, cell_w, cell_h / 2, 255);
        return true;
    }
    if (cp >= 0x2581 && cp <= 0x2588) { /* lower N eighths (2588 = full) */
        int n = (int)(cp - 0x2580); /* 1..8 */
        int y0 = cell_h - (cell_h * n) / 8;
        fill_rect(alpha, cell_w, cell_h, 0, y0, cell_w, cell_h, 255);
        return true;
    }
    if (cp >= 0x2589 && cp <= 0x258F) { /* left N eighths, descending */
        int n = (int)(0x2590 - cp); /* 258F->1 .. 2589->7 */
        int x1 = (cell_w * n) / 8;
        fill_rect(alpha, cell_w, cell_h, 0, 0, x1, cell_h, 255);
        return true;
    }
    if (cp == 0x2590) { /* right half block */
        fill_rect(alpha, cell_w, cell_h, cell_w / 2, 0, cell_w, cell_h, 255);
        return true;
    }
    if (cp >= 0x2591 && cp <= 0x2593) {
        /* Shades: flat partial coverage rather than a dither pattern --
           simpler, and blends fg/bg proportionally much like a real
           terminal renderer's shade characters do at typical font
           sizes; a per-pixel stipple was considered and skipped as
           unnecessary complexity for this pass. */
        static const uint8_t shade[3] = { 64, 128, 192 };
        fill_rect(alpha, cell_w, cell_h, 0, 0, cell_w, cell_h, shade[cp - 0x2591]);
        return true;
    }
    if (cp == 0x2594) { /* upper one eighth block */
        fill_rect(alpha, cell_w, cell_h, 0, 0, cell_w, cell_h / 8, 255);
        return true;
    }
    if (cp == 0x2595) { /* right one eighth block */
        fill_rect(alpha, cell_w, cell_h, cell_w - cell_w / 8, 0, cell_w, cell_h, 255);
        return true;
    }
    if (cp >= 0x2596 && cp <= 0x259F) { /* quadrants */
        static const uint8_t mask[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
        uint8_t m = mask[cp - 0x2596];
        int cx = cell_w / 2, cy = cell_h / 2;
        if (m & 1) fill_rect(alpha, cell_w, cell_h, 0, 0, cx, cy, 255);       /* UL */
        if (m & 2) fill_rect(alpha, cell_w, cell_h, cx, 0, cell_w, cy, 255);  /* UR */
        if (m & 4) fill_rect(alpha, cell_w, cell_h, 0, cy, cx, cell_h, 255);  /* LL */
        if (m & 8) fill_rect(alpha, cell_w, cell_h, cx, cy, cell_w, cell_h, 255); /* LR */
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Legacy computing sextants, U+1FB00-1FB3B                            */
/*                                                                     */
/* Formula and bit layout taken directly from Ghostty's own source     */
/* (src/font/sprite/draw/symbols_for_legacy_computing.zig,             */
/* draw1FB00_1FB3B): the block enumerates all 64 on/off patterns of a  */
/* 2x3 grid in ascending mask order, skipping mask 21 (the left        */
/* column, already U+258C) and mask 42 (the right column, already      */
/* U+2590) since those are representable by existing characters.       */
/* ------------------------------------------------------------------ */

static bool
render_sextant(uint32_t cp, int cell_w, int cell_h, uint8_t *alpha)
{
    if (cp < 0x1fb00 || cp > 0x1fb3b)
        return false;

    uint32_t idx = cp - 0x1fb00;
    uint32_t mask = idx + idx / 20 + 1; /* bit0=tl,1=tr,2=ml,3=mr,4=bl,5=br */

    int cx = cell_w / 2;
    int y0 = cell_h / 3, y1 = (cell_h * 2) / 3;

    if (mask & 1)  fill_rect(alpha, cell_w, cell_h, 0,  0,  cx,      y0,      255); /* tl */
    if (mask & 2)  fill_rect(alpha, cell_w, cell_h, cx, 0,  cell_w,  y0,      255); /* tr */
    if (mask & 4)  fill_rect(alpha, cell_w, cell_h, 0,  y0, cx,      y1,      255); /* ml */
    if (mask & 8)  fill_rect(alpha, cell_w, cell_h, cx, y0, cell_w,  y1,      255); /* mr */
    if (mask & 16) fill_rect(alpha, cell_w, cell_h, 0,  y1, cx,      cell_h,  255); /* bl */
    if (mask & 32) fill_rect(alpha, cell_w, cell_h, cx, y1, cell_w,  cell_h,  255); /* br */
    return true;
}

/* ------------------------------------------------------------------ */
/* Box drawing, U+2500-254B plus single-arm terminators U+2574-257B --  */
/* pure-weight subset only (both light or both heavy per glyph, or a   */
/* single arm for the terminators). Rounded corners (U+256D-2570) are  */
/* handled separately below. Mixed-weight, double-line, dashed, and    */
/* diagonal variants (U+254C-2573, U+257C+) are deliberately out of    */
/* scope for this pass -- see box_draw.h's doc comment and PLAN.md --  */
/* and fall through to the font atlas unchanged (same as before this   */
/* file existed, not a regression).                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t cp;
    uint8_t up, right, down, left; /* 0=none, 1=light, 2=heavy */
} box_line_t;

static const box_line_t BOX_LINES[] = {
    { 0x2500, 0, 1, 0, 1 }, /* ─ */
    { 0x2501, 0, 2, 0, 2 }, /* ━ */
    { 0x2502, 1, 0, 1, 0 }, /* │ */
    { 0x2503, 2, 0, 2, 0 }, /* ┃ */

    { 0x250C, 0, 1, 1, 0 }, /* ┌ */
    { 0x2510, 0, 0, 1, 1 }, /* ┐ */
    { 0x2514, 1, 1, 0, 0 }, /* └ */
    { 0x2518, 1, 0, 0, 1 }, /* ┘ */
    { 0x250F, 0, 2, 2, 0 }, /* ┏ */
    { 0x2513, 0, 0, 2, 2 }, /* ┓ */
    { 0x2517, 2, 2, 0, 0 }, /* ┗ */
    { 0x251B, 2, 0, 0, 2 }, /* ┛ */

    { 0x251C, 1, 1, 1, 0 }, /* ├ */
    { 0x2524, 1, 0, 1, 1 }, /* ┤ */
    { 0x252C, 0, 1, 1, 1 }, /* ┬ */
    { 0x2534, 1, 1, 0, 1 }, /* ┴ */
    { 0x2523, 2, 2, 2, 0 }, /* ┣ */
    { 0x252B, 2, 0, 2, 2 }, /* ┫ */
    { 0x2533, 0, 2, 2, 2 }, /* ┳ */
    { 0x253B, 2, 2, 0, 2 }, /* ┻ */

    { 0x253C, 1, 1, 1, 1 }, /* ┼ */
    { 0x254B, 2, 2, 2, 2 }, /* ╋ */

    /* Single-arm terminators, U+2574-2577/2578-257B -- found live: a
       TUI border used U+2579 (a single short heavy upward tick, no
       down arm) as a border cap/terminator, rendered via the font
       fallback with its usual bearing gap, showing as a short
       disconnected mark below the main vertical line. Only the
       pure-weight ones (matching this table's existing scope) --
       U+257C-257F mix light/heavy per direction and would need
       per-arm thickness, not just per-glyph, so stay deferred. */
    { 0x2574, 0, 0, 0, 1 }, /* ╴ light left */
    { 0x2575, 1, 0, 0, 0 }, /* ╵ light up */
    { 0x2576, 0, 1, 0, 0 }, /* ╶ light right */
    { 0x2577, 0, 0, 1, 0 }, /* ╷ light down */
    { 0x2578, 0, 0, 0, 2 }, /* ╸ heavy left */
    { 0x2579, 2, 0, 0, 0 }, /* ╹ heavy up */
    { 0x257A, 0, 2, 0, 0 }, /* ╺ heavy right */
    { 0x257B, 0, 0, 2, 0 }, /* ╻ heavy down */
};
#define BOX_LINES_COUNT (sizeof(BOX_LINES) / sizeof(BOX_LINES[0]))

static bool
render_box_line(uint32_t cp, int cell_w, int cell_h, uint8_t *alpha)
{
    if (cp < 0x2500 || cp > 0x257b)
        return false;

    const box_line_t *l = NULL;
    for (size_t i = 0; i < BOX_LINES_COUNT; i++) {
        if (BOX_LINES[i].cp == cp) {
            l = &BOX_LINES[i];
            break;
        }
    }
    if (!l)
        return false;

    int min_dim = cell_w < cell_h ? cell_w : cell_h;
    int light_t = min_dim / 8;
    if (light_t < 1) light_t = 1;
    int heavy_t = light_t * 2;

    bool any_heavy = (l->up == 2 || l->right == 2 || l->down == 2 || l->left == 2);
    int t = any_heavy ? heavy_t : light_t;
    int half_t = t / 2;
    if (half_t < 1) half_t = 1;

    int cx = cell_w / 2, cy = cell_h / 2;

    if (l->left)  fill_rect(alpha, cell_w, cell_h, 0,  cy - half_t, cx + half_t, cy + half_t, 255);
    if (l->right) fill_rect(alpha, cell_w, cell_h, cx - half_t, cy - half_t, cell_w, cy + half_t, 255);
    if (l->up)    fill_rect(alpha, cell_w, cell_h, cx - half_t, 0,  cx + half_t, cy + half_t, 255);
    if (l->down)  fill_rect(alpha, cell_w, cell_h, cx - half_t, cy - half_t, cx + half_t, cell_h, 255);
    return true;
}

/* ------------------------------------------------------------------ */
/* Rounded corners, U+256D-2570 (╭╮╯╰)                                  */
/*                                                                     */
/* Ghostty draws these as a stroked bezier curve (src/font/sprite/     */
/* draw/box.zig's arc()) between two points that both sit exactly at   */
/* radius r = min(cell_w,cell_h)/2 from the cell center -- i.e. the    */
/* curve IS a true quarter-circle arc of that radius (the bezier is    */
/* just how their vector canvas draws it), continuing as straight      */
/* arms out to the cell edges beyond the arc's endpoints. Reproduced   */
/* directly here as a circle-band test (squared distances, no libm     */
/* dependency needed) rather than a bezier stroke -- mathematically    */
/* the same curve, simpler for this renderer's plain rectangle-fill    */
/* primitives. */
/* ------------------------------------------------------------------ */

static bool
render_rounded_corner(uint32_t cp, int cell_w, int cell_h, uint8_t *alpha)
{
    /* sx/sy: which side of center this corner's arms extend toward. */
    int sx, sy;
    switch (cp) {
    case 0x256d: sx = 1;  sy = 1;  break; /* ╭ down+right */
    case 0x256e: sx = -1; sy = 1;  break; /* ╮ down+left */
    case 0x256f: sx = -1; sy = -1; break; /* ╯ up+left */
    case 0x2570: sx = 1;  sy = -1; break; /* ╰ up+right */
    default: return false;
    }

    int min_dim = cell_w < cell_h ? cell_w : cell_h;
    int light_t = min_dim / 8;
    if (light_t < 1) light_t = 1;
    int half_t = light_t / 2;
    if (half_t < 1) half_t = 1;

    int cx = cell_w / 2, cy = cell_h / 2;
    int r = min_dim / 2;

    /* Straight arms, beyond where the arc ends. */
    if (sx > 0)
        fill_rect(alpha, cell_w, cell_h, cx + r, cy - half_t, cell_w, cy + half_t, 255);
    else
        fill_rect(alpha, cell_w, cell_h, 0, cy - half_t, cx - r, cy + half_t, 255);
    if (sy > 0)
        fill_rect(alpha, cell_w, cell_h, cx - half_t, cy + r, cx + half_t, cell_h, 255);
    else
        fill_rect(alpha, cell_w, cell_h, cx - half_t, 0, cx + half_t, cy - r, 255);

    /* Arc: the quadrant of the circle (radius r, centered on cx,cy)
       facing this corner's arm directions, stroked to light_t thick. */
    int r_in = r - half_t, r_out = r + half_t;
    long r_in_sq = (long)r_in * r_in, r_out_sq = (long)r_out * r_out;
    for (int y = 0; y < cell_h; y++) {
        if ((sy > 0 && y < cy) || (sy < 0 && y > cy))
            continue;
        for (int x = 0; x < cell_w; x++) {
            if ((sx > 0 && x < cx) || (sx < 0 && x > cx))
                continue;
            long dx = x - cx, dy = y - cy;
            long dist_sq = dx * dx + dy * dy;
            if (dist_sq >= r_in_sq && dist_sq <= r_out_sq)
                alpha[y * cell_w + x] = 255;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */

bool
ghostcon_box_draw_render(uint32_t codepoint, int cell_w, int cell_h, uint8_t *alpha)
{
    memset(alpha, 0, (size_t)cell_w * (size_t)cell_h);

    /* Must precede the box-line check below: rounded corners
       (U+256D-2570) fall inside that range's bounds, and box-line's
       own table lookup would just return false for them (not in
       BOX_LINES), silently skipping the procedural rounded-corner
       path entirely if checked second. */
    if (codepoint >= 0x256d && codepoint <= 0x2570)
        return render_rounded_corner(codepoint, cell_w, cell_h, alpha);
    if (codepoint >= 0x2500 && codepoint <= 0x257b)
        return render_box_line(codepoint, cell_w, cell_h, alpha);
    if (codepoint >= 0x2580 && codepoint <= 0x259f)
        return render_block_element(codepoint, cell_w, cell_h, alpha);
    if (codepoint >= 0x1fb00 && codepoint <= 0x1fb3b)
        return render_sextant(codepoint, cell_w, cell_h, alpha);

    return false;
}
