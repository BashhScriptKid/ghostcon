#include "ghostcon/term/theme.h"

#include <string.h>

typedef struct {
    const char *name;
    GhosttyColorRgb ansi[16];
    GhosttyColorRgb fg, bg, cursor;
} theme_preset_t;

#define RGB(hex) { ((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF }

static const theme_preset_t PRESETS[] = {
    {
        /* Base16 Default Dark (chriskempson/base16-default-scheme),
           standard base16-shell ANSI mapping. */
        .name = "base16-dark",
        .ansi = {
            RGB(0x181818), RGB(0xab4642), RGB(0xa1b56c), RGB(0xf7ca88),
            RGB(0x7cafc2), RGB(0xba8baf), RGB(0x86c1b9), RGB(0xd8d8d8),
            RGB(0x585858), RGB(0xab4642), RGB(0xa1b56c), RGB(0xf7ca88),
            RGB(0x7cafc2), RGB(0xba8baf), RGB(0x86c1b9), RGB(0xf8f8f8),
        },
        .fg = RGB(0xd8d8d8), .bg = RGB(0x181818), .cursor = RGB(0xd8d8d8),
    },
    {
        /* Base16 Default Light -- same accent colors as -dark, the
           grayscale ramp inverts (matches base16's own light variant). */
        .name = "base16-light",
        .ansi = {
            RGB(0xf8f8f8), RGB(0xab4642), RGB(0xa1b56c), RGB(0xf7ca88),
            RGB(0x7cafc2), RGB(0xba8baf), RGB(0x86c1b9), RGB(0x383838),
            RGB(0xb8b8b8), RGB(0xab4642), RGB(0xa1b56c), RGB(0xf7ca88),
            RGB(0x7cafc2), RGB(0xba8baf), RGB(0x86c1b9), RGB(0x181818),
        },
        .fg = RGB(0x383838), .bg = RGB(0xf8f8f8), .cursor = RGB(0x383838),
    },
    {
        /* Solarized (ethanschoonover.com/solarized), dark variant --
           standard xterm/terminal ANSI mapping. */
        .name = "solarized-dark",
        .ansi = {
            RGB(0x073642), RGB(0xdc322f), RGB(0x859900), RGB(0xb58900),
            RGB(0x268bd2), RGB(0xd33682), RGB(0x2aa198), RGB(0xeee8d5),
            RGB(0x002b36), RGB(0xcb4b16), RGB(0x586e75), RGB(0x657b83),
            RGB(0x839496), RGB(0x6c71c4), RGB(0x93a1a1), RGB(0xfdf6e3),
        },
        .fg = RGB(0x839496), .bg = RGB(0x002b36), .cursor = RGB(0x839496),
    },
    {
        /* Same 16-color table as solarized-dark (this is how real
           Solarized works -- only fg/bg swap between variants), light
           background. */
        .name = "solarized-light",
        .ansi = {
            RGB(0x073642), RGB(0xdc322f), RGB(0x859900), RGB(0xb58900),
            RGB(0x268bd2), RGB(0xd33682), RGB(0x2aa198), RGB(0xeee8d5),
            RGB(0x002b36), RGB(0xcb4b16), RGB(0x586e75), RGB(0x657b83),
            RGB(0x839496), RGB(0x6c71c4), RGB(0x93a1a1), RGB(0xfdf6e3),
        },
        .fg = RGB(0x657b83), .bg = RGB(0xfdf6e3), .cursor = RGB(0x657b83),
    },
};

#undef RGB

bool
ghostcon_theme_apply(ghostcon_palette_t *pal, const char *name)
{
    if (!name || !name[0])
        return false;

    for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
        if (strcmp(PRESETS[i].name, name) != 0)
            continue;
        for (int c = 0; c < 16; c++)
            ghostcon_palette_set(pal, c, PRESETS[i].ansi[c]);
        ghostcon_palette_set_default_fg(pal, PRESETS[i].fg);
        ghostcon_palette_set_default_bg(pal, PRESETS[i].bg);
        ghostcon_palette_set_cursor(pal, PRESETS[i].cursor);
        return true;
    }
    return false;
}
