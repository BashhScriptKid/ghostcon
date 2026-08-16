#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ghostty/vt/color.h>

/* 256-color palette + special indices */
#define GC_PALETTE_SIZE 256

/* Default 16 ANSI colors (xterm-compatible) */
extern const GhosttyColorRgb GC_ANSI_COLORS[16];

typedef struct {
    GhosttyColorRgb table[GC_PALETTE_SIZE];
    GhosttyColorRgb fg_default;   /* default foreground */
    GhosttyColorRgb bg_default;   /* default background */
    GhosttyColorRgb cursor_color; /* cursor color */
} ghostcon_palette_t;

void ghostcon_palette_init(ghostcon_palette_t *pal);
void ghostcon_palette_set(ghostcon_palette_t *pal, int idx, GhosttyColorRgb rgb);
void ghostcon_palette_reset(ghostcon_palette_t *pal, uint8_t idx);
GhosttyColorRgb ghostcon_palette_resolve(const ghostcon_palette_t *pal, uint8_t idx);
void ghostcon_palette_set_default_fg(ghostcon_palette_t *pal, GhosttyColorRgb c);
void ghostcon_palette_set_default_bg(ghostcon_palette_t *pal, GhosttyColorRgb c);
void ghostcon_palette_set_cursor(ghostcon_palette_t *pal, GhosttyColorRgb c);

/* ------------------------------------------------------------------ */
/* OSC color-spec parsing (OSC 4/10/11/12) — "#RGB"/"#RRGGBB"/         */
/* "#RRRGGGBBB"/"#RRRRGGGGBBBB" and X11 "rgb:R/G/B" (variable digit    */
/* width per component, 1-4 hex digits each). Named X11 colors         */
/* ("red", "blue", ...) are NOT supported — out of scope, rare in      */
/* real-world OSC usage. The installed libghostty-vt's OSC C API only  */
/* exposes window-title text extraction (verified against both the    */
/* distro package and a from-source master build with                 */
/* -Demit-lib-vt=true) — everything else, including color-spec         */
/* parsing, is ours to do; see term/stream.c's osc_dispatch_manual().  */
/* ------------------------------------------------------------------ */

/* Parses a color-spec string into `out`. Returns false (leaving `out`
   untouched) if `spec` isn't a recognized format. */
bool ghostcon_color_parse_spec(const char *spec, GhosttyColorRgb *out);

/* Formats `c` as an xterm-style query reply value: "rgb:RRRR/GGGG/BBBB"
   (each 8-bit channel replicated to 16 bits, matching real terminals'
   OSC 4/10/11/12 query response convention). `out` must be at least
   19 bytes. */
void ghostcon_color_format_spec(GhosttyColorRgb c, char *out, size_t out_len);
