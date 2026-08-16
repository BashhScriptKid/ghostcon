#include "ghostcon/term/color.h"

#include <stdio.h>
#include <string.h>

const GhosttyColorRgb GC_ANSI_COLORS[16] = {
    {0, 0, 0},       /* black        */
    {170, 0, 0},     /* red          */
    {0, 170, 0},     /* green        */
    {170, 85, 0},    /* yellow       */
    {0, 0, 170},     /* blue         */
    {170, 0, 170},   /* magenta      */
    {0, 170, 170},   /* cyan         */
    {170, 170, 170}, /* white        */
    {85, 85, 85},    /* bright black */
    {255, 85, 85},   /* bright red   */
    {85, 255, 85},   /* bright green */
    {255, 255, 85},  /* bright yellow */
    {85, 85, 255},   /* bright blue  */
    {255, 85, 255},  /* bright magenta */
    {85, 255, 255},  /* bright cyan  */
    {255, 255, 255}, /* bright white */
};

void
ghostcon_palette_init(ghostcon_palette_t *pal) {
    /* ANSI 0-15 */
    for (int i = 0; i < 16; i++)
        pal->table[i] = GC_ANSI_COLORS[i];

    /* 216-color cube (16-231) */
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                pal->table[16 + r * 36 + g * 6 + b] = (GhosttyColorRgb){
                    .r = (uint8_t)(r ? (r * 40 + 55) : 0),
                    .g = (uint8_t)(g ? (g * 40 + 55) : 0),
                    .b = (uint8_t)(b ? (b * 40 + 55) : 0),
                };

    /* Grayscale ramp (232-255) */
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(i * 10 + 8);
        pal->table[232 + i] = (GhosttyColorRgb){.r = v, .g = v, .b = v};
    }

    pal->fg_default = (GhosttyColorRgb){170, 170, 170};
    pal->bg_default = (GhosttyColorRgb){0, 0, 0};
    pal->cursor_color = (GhosttyColorRgb){170, 170, 170};
}

void
ghostcon_palette_set(ghostcon_palette_t *pal, int idx, GhosttyColorRgb rgb) {
    pal->table[idx] = rgb;
}

void
ghostcon_palette_reset(ghostcon_palette_t *pal, uint8_t idx) {
    if (idx < 16)
        pal->table[idx] = GC_ANSI_COLORS[idx];
    else if (idx < 232) {
        int r = (idx - 16) / 36;
        int g = ((idx - 16) % 36) / 6;
        int b = (idx - 16) % 6;
        pal->table[idx] = (GhosttyColorRgb){
            .r = (uint8_t)(r ? (r * 40 + 55) : 0),
            .g = (uint8_t)(g ? (g * 40 + 55) : 0),
            .b = (uint8_t)(b ? (b * 40 + 55) : 0),
        };
    } else {
        uint8_t v = (uint8_t)((idx - 232) * 10 + 8);
        pal->table[idx] = (GhosttyColorRgb){.r = v, .g = v, .b = v};
    }
}

GhosttyColorRgb
ghostcon_palette_resolve(const ghostcon_palette_t *pal, uint8_t idx) {
    return pal->table[idx];
}

void ghostcon_palette_set_default_fg(ghostcon_palette_t *pal, GhosttyColorRgb c) {
    pal->fg_default = c;
}

void ghostcon_palette_set_default_bg(ghostcon_palette_t *pal, GhosttyColorRgb c) {
    pal->bg_default = c;
}

void ghostcon_palette_set_cursor(ghostcon_palette_t *pal, GhosttyColorRgb c) {
    pal->cursor_color = c;
}

static int
hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses `count` hex digits starting at `s` into an 8-bit channel
   value, matching how X11/xterm treat variable-width color components:
   "#f00" and "#ff0000" are both pure red (0xff), not red-scaled-down.
   X11's actual rule (XParseColor) is bit REPLICATION to fill 16 bits,
   not zero-padding: a single digit 0xf becomes 0xffff (repeated, not
   0xf000), so scaling that back down to 8 bits is 0xff, not 0xf0. For
   count==1 that's implemented directly (nibble replicated into both
   halves of the byte); for count>=2 the value already occupies >=8
   bits, so taking the high 8 bits (with count==2 being an exact,
   no-op case) is both correct and what real terminals conventionally
   do. */
static bool
parse_hex_component(const char *s, int count, uint8_t *out)
{
    int val = 0;
    for (int i = 0; i < count; i++) {
        int nib = hex_nibble(s[i]);
        if (nib < 0)
            return false;
        val = (val << 4) | nib;
    }
    if (count == 1) {
        *out = (uint8_t)((val << 4) | val);
    } else {
        int bits = count * 4;
        *out = (uint8_t)(bits > 8 ? (val >> (bits - 8)) : val);
    }
    return true;
}

bool
ghostcon_color_parse_spec(const char *spec, GhosttyColorRgb *out)
{
    size_t len = strlen(spec);

    if (spec[0] == '#') {
        size_t digits = len - 1;
        if (digits == 0 || digits % 3 != 0 || digits > 12)
            return false;
        int per = (int)(digits / 3);
        GhosttyColorRgb c;
        if (!parse_hex_component(spec + 1, per, &c.r) ||
            !parse_hex_component(spec + 1 + per, per, &c.g) ||
            !parse_hex_component(spec + 1 + 2 * per, per, &c.b))
            return false;
        *out = c;
        return true;
    }

    if (strncmp(spec, "rgb:", 4) == 0) {
        const char *p = spec + 4;
        GhosttyColorRgb c;
        uint8_t *comps[3] = { &c.r, &c.g, &c.b };
        for (int i = 0; i < 3; i++) {
            const char *start = p;
            while (*p != '\0' && *p != '/')
                p++;
            int n = (int)(p - start);
            if (n == 0 || n > 4 || !parse_hex_component(start, n, comps[i]))
                return false;
            if (i < 2) {
                if (*p != '/')
                    return false;
                p++;
            }
        }
        if (*p != '\0') /* trailing garbage after the third component */
            return false;
        *out = c;
        return true;
    }

    return false; /* named X11 colors unsupported -- see header comment */
}

void
ghostcon_color_format_spec(GhosttyColorRgb c, char *out, size_t out_len)
{
    snprintf(out, out_len, "rgb:%02x%02x/%02x%02x/%02x%02x",
              c.r, c.r, c.g, c.g, c.b, c.b);
}
