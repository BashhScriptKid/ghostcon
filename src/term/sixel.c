#include "ghostcon/term/sixel.h"

#include <stdlib.h>
#include <string.h>

void
ghostcon_sixel_state_init(ghostcon_sixel_state_t *st)
{
    memset(st, 0, sizeof *st);
}

void
ghostcon_sixel_state_deinit(ghostcon_sixel_state_t *st)
{
    for (int i = 0; i < GHOSTCON_SIXEL_MAX_PLACEMENTS; i++)
        free(st->placements[i].pixels);
    memset(st, 0, sizeof *st);
}

static ghostcon_sixel_placement_t *
alloc_placement_slot(ghostcon_sixel_state_t *st)
{
    for (int i = 0; i < GHOSTCON_SIXEL_MAX_PLACEMENTS; i++) {
        if (!st->placements[i].in_use) {
            memset(&st->placements[i], 0, sizeof st->placements[i]);
            st->placements[i].in_use = true;
            return &st->placements[i];
        }
    }
    /* No free slot -- sixel has no ack/error-reporting channel back
       to the client at all (unlike Kitty's ENOSPC ack), so silently
       dropping a new image would be the only alternative to eviction.
       Evicting the lowest-index in-use slot isn't strict LRU, but
       with 64 slots this only matters for a client placing an
       unusually large number of live sixel images at once, and "the
       oldest-ish one disappears to make room for the newest" is a
       reasonable, unsurprising fallback -- matching the spirit of how
       a real terminal's finite backing store behaves under pressure. */
    ghostcon_sixel_placement_t *victim = &st->placements[0];
    if (st->total_bytes >= victim->pixel_len)
        st->total_bytes -= victim->pixel_len;
    free(victim->pixels);
    memset(victim, 0, sizeof *victim);
    victim->in_use = true;
    return victim;
}

typedef struct {
    uint8_t r, g, b;
    bool    defined;
} color_reg_t;

/* Parses a `#Pc[;Pu;Px;Py;Pz]` color-introducer command starting right
   after the '#'. Advances *p past the command. Pu=1 is HLS
   (Px=hue 0-360, Py=lightness 0-100, Pz=saturation 0-100), Pu=2 is RGB
   (Px,Py,Pz all 0-100 percent) -- matches the encoder in
   tools/image_protocol_bench.c's sixel_send(), which always emits
   Pu=2 (RGB), and real-world encoders (img2sixel) do the same. HLS is
   supported anyway since it's part of the spec and some encoders can
   use it. */
static void
parse_color_intro(const char **p, const char *end, color_reg_t *regs,
                  int *current_reg)
{
    const char *s = *p;
    long vals[5];
    int nvals = 0;
    while (nvals < 5) {
        char *after;
        long v = strtol(s, &after, 10);
        if (after == s)
            break;
        vals[nvals++] = v;
        s = after;
        if (*s == ';' && s < end)
            s++;
        else
            break;
    }
    *p = s;
    if (nvals < 1)
        return;
    int pc = (int)vals[0];
    if (pc < 0 || pc >= GHOSTCON_SIXEL_COLOR_REGISTERS)
        return;
    *current_reg = pc;
    if (nvals < 5)
        return; /* select only, no (re)definition */

    long pu = vals[1], px = vals[2], py = vals[3], pz = vals[4];
    if (pu == 2) {
        /* RGB, each component 0-100 percent */
        px = px < 0 ? 0 : px > 100 ? 100 : px;
        py = py < 0 ? 0 : py > 100 ? 100 : py;
        pz = pz < 0 ? 0 : pz > 100 ? 100 : pz;
        regs[pc].r = (uint8_t)((px * 255 + 50) / 100);
        regs[pc].g = (uint8_t)((py * 255 + 50) / 100);
        regs[pc].b = (uint8_t)((pz * 255 + 50) / 100);
        regs[pc].defined = true;
    } else if (pu == 1) {
        /* HLS: hue 0-360, lightness/saturation 0-100 */
        double h = (double)(((px % 360) + 360) % 360);
        double l = (double)(py < 0 ? 0 : py > 100 ? 100 : py) / 100.0;
        double s_ = (double)(pz < 0 ? 0 : pz > 100 ? 100 : pz) / 100.0;
        double c = (1.0 - (l < 0.5 ? 1.0 - 2.0 * l : 2.0 * l - 1.0)) * s_;
        double hp = h / 60.0;
        double hp_mod2 = hp - 2.0 * (double)((long)(hp / 2.0)); /* fmod(hp, 2) */
        double x = c * (1.0 - (hp_mod2 > 1.0 ? 2.0 - hp_mod2 : hp_mod2));
        double r1, g1, b1;
        if (hp < 1)      { r1 = c; g1 = x; b1 = 0; }
        else if (hp < 2) { r1 = x; g1 = c; b1 = 0; }
        else if (hp < 3) { r1 = 0; g1 = c; b1 = x; }
        else if (hp < 4) { r1 = 0; g1 = x; b1 = c; }
        else if (hp < 5) { r1 = x; g1 = 0; b1 = c; }
        else             { r1 = c; g1 = 0; b1 = x; }
        double m = l - c / 2.0;
        regs[pc].r = (uint8_t)(((r1 + m) * 255.0) + 0.5);
        regs[pc].g = (uint8_t)(((g1 + m) * 255.0) + 0.5);
        regs[pc].b = (uint8_t)(((b1 + m) * 255.0) + 0.5);
        regs[pc].defined = true;
    }
}

void
ghostcon_sixel_decode(ghostcon_sixel_state_t *st,
                      const int32_t *dcs_params, size_t params_count,
                      const char *body, size_t body_len,
                      int32_t cursor_col, int32_t cursor_row,
                      int32_t cell_w, int32_t cell_h,
                      ghostcon_sixel_cursor_move_t *out_move)
{
    if (out_move)
        memset(out_move, 0, sizeof *out_move);

    /* P2 (background select): 1 = positions never touched by any
       sixel character stay transparent (the underlying cell content
       shows through); 0/2/absent = untouched positions are filled
       with an opaque background. Real terminals fill with the
       terminal's actual current background color for P2=0; this
       module is decoupled from screen state (see this file's own
       header doc comment) the same way kitty_graphics.h is, so there
       is no background color to reach for here -- opaque black is
       used instead, matching what most sixel-producing tools already
       expect in practice (they fill their own canvas fully rather
       than relying on this corner of the spec). */
    int32_t p2 = params_count > 1 ? dcs_params[1] : 0;
    bool transparent_bg = (p2 == 1);

    /* First pass: scan for a raster-attribute command ("Pan;Pad;Ph;Pv)
       to get a declared size hint, and separately find the true
       extent by tracking max x reached and max y-band reached while
       walking the sixel character stream -- some encoders declare a
       size that doesn't match what they actually emit, so the final
       canvas uses whichever is larger, same "trust but verify"
       posture as kitty_graphics.c's PNG width/height handling. */
    int32_t declared_w = 0, declared_h = 0;
    int32_t max_x = 0, max_y_band_end = 0;
    {
        const char *p = body, *end = body + body_len;
        int32_t x = 0, y0 = 0;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                p++;
                long vals[4]; int n = 0;
                while (n < 4 && p < end) {
                    char *after;
                    long v = strtol(p, &after, 10);
                    if (after == p) break;
                    vals[n++] = v;
                    p = after;
                    if (p < end && *p == ';') p++;
                    else break;
                }
                if (n >= 4) { declared_w = (int32_t)vals[2]; declared_h = (int32_t)vals[3]; }
            } else if (c == '#') {
                p++;
                while (p < end && ((*p >= '0' && *p <= '9') || *p == ';'))
                    p++;
            } else if (c == '!') {
                p++;
                char *after;
                long rep = strtol(p, &after, 10);
                p = (after == p) ? p + 1 : after;
                if (p < end && *p >= 0x3F && *p <= 0x7E) {
                    x += (int32_t)(rep > 0 ? rep : 1);
                    p++;
                }
            } else if (c >= 0x3F && c <= 0x7E) {
                x++;
                p++;
            } else if (c == '$') {
                if (x > max_x) max_x = x;
                x = 0;
                p++;
            } else if (c == '-') {
                if (x > max_x) max_x = x;
                y0 += 6;
                if (y0 > max_y_band_end) max_y_band_end = y0;
                x = 0;
                p++;
            } else {
                p++;
            }
        }
        if (x > max_x) max_x = x;
        int32_t last_band_end = y0 + 6;
        if (last_band_end > max_y_band_end) max_y_band_end = last_band_end;
    }

    int32_t width = declared_w > max_x ? declared_w : max_x;
    /* Trust declared_h outright when present, rather than taking
       max(declared_h, max_y_band_end): max_y_band_end is rounded UP
       to the nearest multiple of 6 (sixel's row-band height), so for
       any declared height that isn't itself a multiple of 6 (e.g.
       200), max_y_band_end (204) would always appear "bigger" and
       silently inflate the canvas by up to 5 extra blank rows even
       though the raster attribute -- which real encoders compute
       exactly -- already says how tall the image actually is. A sixel
       stream legitimately containing more row-bands than declared is
       still handled safely: the paint pass below bounds-checks every
       write against `height`, so excess bands are just clipped, the
       same way a real terminal clips to the declared raster size.
       max_y_band_end is only used as a fallback when no raster
       attribute was sent at all (declared_h <= 0). */
    int32_t height = declared_h > 0 ? declared_h : max_y_band_end;
    if (width <= 0 || height <= 0 ||
        (uint32_t)width > GHOSTCON_SIXEL_MAX_DIM ||
        (uint32_t)height > GHOSTCON_SIXEL_MAX_DIM)
        return; /* nothing decodable -- silently ignore, no ack channel to report on */

    size_t pixel_len = (size_t)width * (size_t)height * 4;
    if (pixel_len > GHOSTCON_SIXEL_MAX_IMAGE_BYTES)
        return;

    uint8_t *pixels = malloc(pixel_len);
    if (!pixels)
        return;
    /* Fill with the P2-determined default: transparent (alpha 0) or
       opaque black -- see the P2 doc comment above. */
    if (transparent_bg) {
        memset(pixels, 0, pixel_len);
    } else {
        for (size_t i = 0; i < pixel_len; i += 4) {
            pixels[i + 0] = 0; pixels[i + 1] = 0; pixels[i + 2] = 0; pixels[i + 3] = 255;
        }
    }

    /* Second pass: actually paint. Registers start undefined (black,
       per this module's decoupled-from-screen-state posture -- see
       parse_color_intro's own doc comment); every real-world encoder
       defines every register it uses before selecting it, so this
       only matters for a pathological stream that selects a register
       it never defined. */
    color_reg_t regs[GHOSTCON_SIXEL_COLOR_REGISTERS];
    memset(regs, 0, sizeof regs);
    int current_reg = 0;

    const char *p = body, *end = body + body_len;
    int32_t x = 0, y0 = 0;
    while (p < end) {
        char c = *p;
        if (c == '"') {
            p++;
            int n = 0;
            while (n < 4 && p < end) {
                char *after;
                strtol(p, &after, 10);
                if (after == p) break;
                n++;
                p = after;
                if (p < end && *p == ';') p++;
                else break;
            }
        } else if (c == '#') {
            p++;
            parse_color_intro(&p, end, regs, &current_reg);
        } else if (c == '!') {
            p++;
            char *after;
            long rep = strtol(p, &after, 10);
            if (after == p) rep = 1;
            p = after;
            if (p < end && *p >= 0x3F && *p <= 0x7E) {
                uint8_t bits = (uint8_t)(*p - 0x3F);
                color_reg_t *reg = &regs[current_reg];
                for (long k = 0; k < rep; k++) {
                    if (x >= 0 && x < width) {
                        for (int r = 0; r < 6; r++) {
                            if (bits & (1u << r)) {
                                int32_t py = y0 + r;
                                if (py >= 0 && py < height) {
                                    size_t off = ((size_t)py * (size_t)width + (size_t)x) * 4;
                                    pixels[off + 0] = reg->r;
                                    pixels[off + 1] = reg->g;
                                    pixels[off + 2] = reg->b;
                                    pixels[off + 3] = 255;
                                }
                            }
                        }
                    }
                    x++;
                }
                p++;
            }
        } else if (c >= 0x3F && c <= 0x7E) {
            uint8_t bits = (uint8_t)(c - 0x3F);
            if (x >= 0 && x < width) {
                color_reg_t *reg = &regs[current_reg];
                for (int r = 0; r < 6; r++) {
                    if (bits & (1u << r)) {
                        int32_t py = y0 + r;
                        if (py >= 0 && py < height) {
                            size_t off = ((size_t)py * (size_t)width + (size_t)x) * 4;
                            pixels[off + 0] = reg->r;
                            pixels[off + 1] = reg->g;
                            pixels[off + 2] = reg->b;
                            pixels[off + 3] = 255;
                        }
                    }
                }
            }
            x++;
            p++;
        } else if (c == '$') {
            x = 0;
            p++;
        } else if (c == '-') {
            y0 += 6;
            x = 0;
            p++;
        } else {
            p++; /* whitespace/unrecognized -- ignore */
        }
    }

    if (st->total_bytes + pixel_len > GHOSTCON_SIXEL_MAX_TOTAL_BYTES) {
        free(pixels);
        return;
    }

    ghostcon_sixel_placement_t *placement = alloc_placement_slot(st);
    placement->width = width;
    placement->height = height;
    placement->pixels = pixels;
    placement->pixel_len = pixel_len;
    placement->anchor_col = cursor_col;
    placement->anchor_row = cursor_row;
    placement->generation++;
    st->total_bytes += pixel_len;

    if (out_move && cell_w > 0 && cell_h > 0) {
        int32_t rows = (height + cell_h - 1) / cell_h;
        int32_t cols = (width + cell_w - 1) / cell_w;
        out_move->moved = true;
        out_move->rows = rows;
        out_move->col = cursor_col + cols;
    }
}
