#include "ghostcon/render/ligature.h"

#include "ghostcon/term/cell.h"

#include <stdbool.h>
#include <string.h>

/* Operator/punctuation characters actual programming-ligature fonts
 * (JetBrains Mono, Fira Code, Cascadia Code, ...) key their calt/liga
 * substitutions off of. Deliberately generous -- a false-positive candidate
 * just costs one wasted hb_shape() call that reports "no substitution",
 * same as any character never in this set. Not meant to be exhaustive; the
 * real gate is HarfBuzz's own output, not this list.
 */
static const char ligature_trigger_chars[] = "!#$%&*+-./:<=>?@^|~";

static bool
is_ligature_trigger(uint32_t cp) {
    if (cp == 0 || cp > 0x7F)
        return false;
    return memchr(ligature_trigger_chars, (int)cp, sizeof(ligature_trigger_chars) - 1) != NULL;
}

static uint32_t
cell_codepoint_or_zero(ghostcon_cell_t cell) {
    ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(cell);
    return (tag == GHOSTCON_CELL_CODEPOINT || tag == GHOSTCON_CELL_CODEPOINT_GRAPHEME)
        ? ghostcon_cell_get_codepoint(cell) : 0;
}

int
ghostcon_ligature_scan_row(const ghostcon_row_t *row, ghostcon_atlas_t *atlas,
                            ghostcon_ligature_glyph_t *out_glyphs, int max_glyphs)
{
    int n_out = 0;
    uint16_t x = 0;

    while (x < row->cols && n_out < max_glyphs) {
        uint32_t cp = cell_codepoint_or_zero(row->cells[x]);
        if (!is_ligature_trigger(cp)) {
            x++;
            continue;
        }

        uint16_t run_start = x;
        uint16_t style = ghostcon_cell_get_style(row->cells[x]);
        uint32_t cps[GHOSTCON_LIGATURE_MAX_RUN_LEN];
        int run_len = 0;

        while (x < row->cols && run_len < GHOSTCON_LIGATURE_MAX_RUN_LEN) {
            ghostcon_cell_t c = row->cells[x];
            uint32_t c_cp = cell_codepoint_or_zero(c);
            if (!is_ligature_trigger(c_cp) || ghostcon_cell_get_style(c) != style)
                break;
            cps[run_len++] = c_cp;
            x++;
        }

        /* A single trigger char alone (e.g. a lone "+") never triggers a
         * contextual substitution on its own -- don't bother shaping.
         */
        if (run_len < 2)
            continue;

        uint32_t out_gidx[GHOSTCON_LIGATURE_MAX_RUN_LEN];
        float out_adv[GHOSTCON_LIGATURE_MAX_RUN_LEN];
        int shaped = ghostcon_atlas_shape_run(atlas, cps, run_len, out_gidx, out_adv,
                                               GHOSTCON_LIGATURE_MAX_RUN_LEN);
        (void)out_adv;

        /* Only trust a straight 1:1 mapping -- see ligature.h's own doc
         * comment on ghostcon_ligature_scan_row() for why a glyph COUNT
         * mismatch (an actual multi-codepoint collapse or reordering) is
         * left unreported rather than guessed at.
         */
        if (shaped == run_len) {
            for (int i = 0; i < run_len && n_out < max_glyphs; i++) {
                uint32_t plain = ghostcon_atlas_plain_glyph_index(atlas, cps[i]);
                if (out_gidx[i] != plain && out_gidx[i] != 0) {
                    out_glyphs[n_out++] = (ghostcon_ligature_glyph_t){
                        .x = (uint16_t)(run_start + i),
                        .glyph_index = out_gidx[i],
                        .run_start = run_start,
                        .run_len = (uint16_t)run_len,
                    };
                }
            }
        }
    }

    return n_out;
}
