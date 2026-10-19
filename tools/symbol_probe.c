/* symbol_probe -- prints a batch of codepoints as raw UTF-8 text, one per
 * line, alongside ghostcon's own ghostcon_cell_is_symbol()/
 * is_graphics_element() classification -- meant to be piped or pasted into
 * a REAL Ghostty window so its rendering can be eyeballed side-by-side with
 * what ghostcon decided, as an independent check on the UCD-generated table
 * + the two hand-added carve-outs (src/term/cell_symbol_table.gen.c,
 * src/term/cell.c).
 *
 * Not a meson test target (nothing to assert against here
 *
 * this is for human eyes against a live reference terminal, not CI).
 *
 * Usage:
 *   symbol_probe             curated set: known-problem glyphs plus
 *                             one sample per notable symbol block
 *   symbol_probe --boundaries  lo-1/lo/hi/hi+1 around every merged
 *                             generated-table range AND both carve-out
 *                             edges (Box Drawing, PUA) -- the cases
 *                             most likely to reveal an off-by-one
 */

#include "ghostcon/term/cell.h"
#include "ghostcon/term/cell_symbol_table.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
utf8_encode(uint32_t cp, char *buf)
{
    if (cp < 0x80)
    {
        buf[0] = (char)cp;
        return 1;
    }
    else if (cp < 0x800)
    {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    else if (cp < 0x10000)
    {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    else
    {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static void print_cp(uint32_t cp, const char *label)
{
    char utf8[5] = {0};
    /* U+D800-DFFF are UTF-16 surrogates
     *
     * not valid Unicode scalar values, so encoding one as UTF-8 would emit
     * garbage bytes that could desync a paste into a real terminal. is_symbol()
     * itself operates on raw uint32_t with no such restriction, so the
     * classification call below still runs
     *
     * only the printed glyph is substituted.
     */
    if (cp >= 0xD800 && cp <= 0xDFFF)
    {
        snprintf(utf8, sizeof(utf8), "?");
    }
    else
    {
        int n = utf8_encode(cp, utf8);
        utf8[n] = '\0';
    }
    bool sym = ghostcon_cell_is_symbol(cp);
    bool graphics = ghostcon_cell_is_graphics_element(cp);
    printf("U+%04X  %s  ghostcon=%s%s  %s\n",
           cp, utf8, sym ? "symbol" : "not-symbol",
           graphics ? "+graphics-element" : "",
           label ? label : "");
}

typedef struct
{
    uint32_t cp; const char *label;
} probe_entry_t;

static const probe_entry_t curated[] = {
    /* Known-problem glyphs that motivated is_symbol() in the first place */
    { 0x2299, "CIRCLED DOT OPERATOR (found overflowing live)" },
    { 0x25C9, "FISHEYE (found overflowing live)" },
    { 0x25CE, "BULLSEYE (found overflowing live)" },
    { 0xF407, "Nerd Font git-pull-request octicon (PUA)" },
    /* One sample per notable Symbol block */
    { 0x2103, "Letterlike Symbols (DEGREE CELSIUS)" },
    { 0x2192, "Arrows (RIGHTWARDS ARROW)" },
    { 0x2318, "Misc Technical (PLACE OF INTEREST SIGN)" },
    { 0x24B6, "Enclosed Alphanumerics (CIRCLED A)" },
    { 0x2705, "Dingbats/emoji (WHITE HEAVY CHECK MARK)" },
    { 0x2B50, "Misc Symbols and Arrows (STAR)" },
    { 0x1FB00, "Symbols for Legacy Computing (sextant)" },
    /* Carve-outs */
    { 0x2500, "Box Drawing -- excluded despite being category So" },
    { 0x2588, "Block Element -- excluded despite being category So" },
    { 0xE0B2, "Powerline glyph (PUA carve-out, also graphics-element)" },
    { 0xE000, "start of PUA -- included despite being category Co" },
    /* Wide emoji -- newly classified as symbol since the UCD swap */
    { 0x1F600, "GRINNING FACE -- genuinely category So (live-verified: no clipping)" },
    { 0x1F680, "ROCKET" },
    /* Negative controls */
    { 'A', "plain ASCII letter" },
    { 0x4E2D, "CJK ideograph (category Lo, not Symbol)" },
};

static void print_boundaries(void)
{
    printf("--- generated-table range boundaries ---\n");
    for (size_t i = 0; i < ghostcon_generated_symbol_ranges_count; i++)
    {
        uint32_t lo = ghostcon_generated_symbol_ranges[i].lo;
        uint32_t hi = ghostcon_generated_symbol_ranges[i].hi;
        if (lo > 0) print_cp(lo - 1, "range lo-1");
        print_cp(lo, "range lo");
        print_cp(hi, "range hi");
        if (hi < 0x10FFFF) print_cp(hi + 1, "range hi+1");
    }
    printf("--- carve-out boundaries ---\n");
    print_cp(0x24FF, "before Box Drawing carve-out");
    print_cp(0x2500, "Box Drawing carve-out start");
    print_cp(0x259F, "Box Drawing carve-out end");
    print_cp(0x25A0, "after Box Drawing carve-out");
    print_cp(0xDFFF, "before PUA carve-out");
    print_cp(0xE000, "PUA carve-out start");
    print_cp(0xF8FF, "PUA carve-out end");
    print_cp(0xF900, "after PUA carve-out");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--boundaries") == 0)
    {
        print_boundaries();
        return 0;
    }
    for (size_t i = 0; i < sizeof(curated) / sizeof(curated[0]); i++)
    {
        print_cp(curated[i].cp, curated[i].label);
    }
    return 0;
}
