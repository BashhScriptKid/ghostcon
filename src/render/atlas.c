#include "ghostcon/render/atlas.h"
#include "ghostcon/render/box_draw.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-size open-addressing hash table. Terminal glyph sets are small (ASCII + a
 * modest working set of Unicode)
 *
 * no need for a resizable map.
 *
 * GHOSTCON_ATLAS_MAX_GLYPHS is generous headroom, not a hard cap on what any one
 * session can display (a full atlas just means new glyphs stop being cached and
 * lose the ability to render, which is the same failure mode as the atlas bitmap
 * itself filling up).
 */
#define GHOSTCON_ATLAS_MAX_GLYPHS 4096

/* Gap (pixels) left after every packed glyph, both right and below. gles.c
 * samples the atlas with GL_LINEAR; with zero gap, a glyph's own edge texel gets
 * bilinearly blended with whatever unrelated glyph happens to be packed
 * immediately next to it in the atlas --a bleed artifact that reads as faint
 * fringing/ghosting on glyph edges (found live: visible once the gamma-correction
 * fix made edges crisp enough for the bleed to stand out from the noise).
 */
#define GHOSTCON_ATLAS_GLYPH_PADDING 1

/* Bound for a color bitmap glyph's rasterized size
 *
 * see this file's color-loading path in ghostcon_atlas_glyph() for why this needs
 * to be wider than the mono/outline path's 128x128 cap (a color font's fixed
 * strike size, not the target font_size_px).
 *
 * Still headroom, not a real limit
 *
 * Noto Color Emoji's actual strike is 136x128.
 */
#define GHOSTCON_ATLAS_COLOR_GLYPH_MAX 160

typedef struct
{
    uint32_t codepoint; /* 0 = empty slot (codepoint 0 / NUL is never rendered) */
    bool     occupied;
    ghostcon_glyph_t glyph;
} atlas_slot_t;

struct ghostcon_atlas {
    FT_Library ft;
    FT_Face    face;

    /* HarfBuzz view of `face` above, for ligature shaping (see
     * ghostcon_atlas_shape_run())
     *
     * NULL if harfbuzz wasn't available at build time (dep_harfbuzz is `required:
     * false`), in which case shaping is simply unavailable and
     * render/ligature.c's caller treats every run as "not a real ligature", same
     * as any other codepoint the font doesn't substitute.
     * hb_ft_font_create_referenced keeps its own reference to `face`, independent
     * of this struct's own FT_Done_Face in ghostcon_atlas_destroy().
     */
    hb_font_t *hb_font;

    uint32_t dim;
    uint8_t *bitmap; /* dim * dim * 3, RGB (one byte per channel) per pixel */
    bool     dirty;

    /* Shelf packer state */
    uint32_t pen_x, pen_y, shelf_h;

    /* Color glyph atlas
     *
     * separate bitmap/packer/dirty flag, same dim as the mask atlas above.
     *
     * See atlas.h's doc comment on ghostcon_glyph_t.is_color for why this can't
     * share the mask atlas's storage or packer state.
     */
    uint8_t *color_bitmap; /* dim * dim * 4, RGBA (straight alpha) */
    bool     color_dirty;
    uint32_t color_pen_x, color_pen_y, color_shelf_h;

    int cell_w, cell_h;
    int ascent; /* baseline offset from cell top, pixels */

    /* Font fallback chain: when the primary face (above) lacks a glyph,
     * ghostcon_atlas_glyph() walks this system-provided fallback order instead of
     * just giving up. fallback_set is fontconfig's own FcFontSort() result for
     * the SAME pattern the primary face was matched from
     *
     * the same ordered candidate list a normal desktop app gets "for free" via
     * its toolkit, not a hardcoded list this project would have to maintain and
     * which would inevitably miss whatever fonts are actually installed on a
     * given system.
     *
     * Faces are opened lazily (most users never need more than the first one or
     * two candidates that actually cover their content) and cached in
     * fallback_faces, parallel-indexed to fallback_set; a NULL entry means "not
     * tried yet", (FT_Face)(intptr_t)-1 means "tried and failed to open" so a
     * broken font file in the list isn't retried on every miss.
     *
     * Found live: the default font had no glyph at all for several
     * emoji/Nerd-Font-icon codepoints, and ghostcon_atlas_glyph() just returned
     * NULL
     *
     * the renderer skipped drawing anything rather than trying any other
     * installed font that DID have the glyph.
     */
    FcFontSet *fallback_set;
    FT_Face   *fallback_faces;
    int        font_size_px;

    /* "grayscale" (default)/"subpixel"/"cleartype"/"none"
     *
     * see atlas.h's own doc comment on ghostcon_atlas_create()'s antialiasing
     * parameter for the full explanation.
     */
    enum { GC_ATLAS_AA_GRAYSCALE, GC_ATLAS_AA_SUBPIXEL, GC_ATLAS_AA_NONE,
           GC_ATLAS_AA_CLEARTYPE } aa_mode;
    bool bgr_order; /* only meaningful when aa_mode == GC_ATLAS_AA_CLEARTYPE */

    atlas_slot_t slots[GHOSTCON_ATLAS_MAX_GLYPHS];
};

/* Sentinel stored in fallback_faces[i] for "already tried to open this candidate
 * face and failed"
 *
 * distinct from NULL ("not tried yet") so a broken/unreadable font file in the
 * fallback list only ever gets one failed FT_New_Face() attempt, not one per
 * glyph miss for the rest of the session.
 */
#define GHOSTCON_ATLAS_FACE_FAILED ((FT_Face)(intptr_t)-1)

static atlas_slot_t
*slot_find(ghostcon_atlas_t *atlas, uint32_t codepoint, bool *found)
{
    uint32_t h = codepoint * 2654435761u; /* Knuth multiplicative hash */
    for (uint32_t i = 0; i < GHOSTCON_ATLAS_MAX_GLYPHS; i++)
    {
        uint32_t idx = (h + i) % GHOSTCON_ATLAS_MAX_GLYPHS;
        atlas_slot_t *s = &atlas->slots[idx];
        if (!s->occupied)
        {
            *found = false;
            return s;
        }
        if (s->codepoint == codepoint)
        {
            *found = true;
            return s;
        }
    }
    *found = false;
    return NULL; /* table full */
}

ghostcon_atlas_t
*ghostcon_atlas_create(const char *font_family, const char *font_variant, const char *antialiasing, const char *subpixel_order, int font_size_px, uint32_t atlas_dim)
{
    ghostcon_atlas_t *atlas = calloc(1, sizeof(*atlas));
    if (!atlas)
        return NULL;

    atlas->aa_mode = GC_ATLAS_AA_GRAYSCALE;
    if (antialiasing)
    {
        if (strcmp(antialiasing, "subpixel") == 0)
            atlas->aa_mode = GC_ATLAS_AA_SUBPIXEL;
        else if (strcmp(antialiasing, "cleartype") == 0)
            atlas->aa_mode = GC_ATLAS_AA_CLEARTYPE;
        else if (strcmp(antialiasing, "none") == 0)
            atlas->aa_mode = GC_ATLAS_AA_NONE;
        /* anything else (including "grayscale" itself, or an unrecognized value)
         *
         * already defaulted above, not an error
         */
    }
    atlas->bgr_order = subpixel_order && strcmp(subpixel_order, "bgr") == 0;

    if (!FcInit())
        goto fail;

    FcPattern *pat = FcPatternCreate();
    if (font_family)
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)font_family);
    if (font_variant && font_variant[0])
        FcPatternAddString(pat, FC_STYLE, (const FcChar8 *)font_variant);
    FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    if (!match)
    {
        FcPatternDestroy(pat);
        goto fail;
    }

    FcChar8 *file_path = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file_path) != FcResultMatch)
    {
        FcPatternDestroy(match);
        FcPatternDestroy(pat);
        goto fail;
    }

    /* Same pattern the primary match above came from, but sorted (not just
     * best-matched)
     *
     * fontconfig's own system fallback order, respecting whatever fallback config
     * this machine actually has.
     *
     * Best-effort: a NULL result here just means no fallback chain
     * (atlas->fallback_set stays NULL, glyph lookup falls back to "primary lacks
     * it -> give up", same as before this feature existed), not a fatal error for
     * the whole atlas.
     */
    FcResult sort_result;
    atlas->fallback_set = FcFontSort(NULL, pat, FcTrue, NULL, &sort_result);
    FcPatternDestroy(pat);
    if (atlas->fallback_set && atlas->fallback_set->nfont > 0)
    {
        atlas->fallback_faces = (FT_Face *)calloc((size_t)atlas->fallback_set->nfont,
                                                       sizeof(FT_Face));
        if (!atlas->fallback_faces)
        {
            FcFontSetDestroy(atlas->fallback_set);
            atlas->fallback_set = NULL;
        }
    }

    if (FT_Init_FreeType(&atlas->ft))
    {
        FcPatternDestroy(match);
        goto fail;
    }
    if (FT_New_Face(atlas->ft, (const char *)file_path, 0, &atlas->face))
    {
        FcPatternDestroy(match);
        goto fail;
    }
    FcPatternDestroy(match);

    FT_Set_Pixel_Sizes(atlas->face, 0, (FT_UInt)font_size_px);
    atlas->font_size_px = font_size_px;

    /* Monospace cell size: advance of a representative ASCII glyph, and the
     * face's line height.
     */
    if (FT_Load_Char(atlas->face, 'M', FT_LOAD_DEFAULT) == 0)
        atlas->cell_w = (int)(atlas->face->glyph->advance.x >> 6);
    if (atlas->cell_w <= 0)
        atlas->cell_w = font_size_px / 2 + 1;
    atlas->cell_h = (int)(atlas->face->size->metrics.height >> 6);
    if (atlas->cell_h <= 0)
        atlas->cell_h = font_size_px + 2;

    /* Baseline offset from the cell's top edge.
     *
     * Must come from the font's line metrics (ascender), NOT from any per-glyph
     * bearing
     *
     * anchoring per-glyph would put descenders (g, q, y, p, j) at a different
     * baseline than the rest of the line and, worse, using cell_h itself as the
     * anchor (as an earlier version of this code did) leaves zero room below the
     * baseline, so descenders spill into the next row and get overdrawn by its
     * background quad.
     */
    atlas->ascent = (int)(atlas->face->size->metrics.ascender >> 6);
    if (atlas->ascent <= 0)
        atlas->ascent = atlas->cell_h * 4 / 5; /* rough fallback */

    atlas->dim = atlas_dim;
    atlas->bitmap = calloc(1, (size_t)atlas_dim * atlas_dim * 3);
    if (!atlas->bitmap)
        goto fail_face;
    atlas->color_bitmap = calloc(1, (size_t)atlas_dim * atlas_dim * 4);
    if (!atlas->color_bitmap)
        goto fail_face;

    /* Reserve a 2x2 opaque block at the atlas origin for solid-color quads
     * (background cells, cursor)
     *
     * see ghostcon_atlas_reserved_uv() and render/gles.c's push_rect.
     *
     * Advance the shelf packer past it so no real glyph ever lands there. 3
     * bytes/pixel now (RGB atlas).
     */
    memset(atlas->bitmap, 0xFF, 6);                                   /* row 0: (0,0) (1,0) */
    memset(atlas->bitmap + (size_t)atlas_dim * 3, 0xFF, 6);           /* row 1: (0,1) (1,1) */
    atlas->pen_x = 2 + GHOSTCON_ATLAS_GLYPH_PADDING;
    atlas->shelf_h = 2;

    atlas->dirty = true;

    atlas->hb_font = hb_ft_font_create_referenced(atlas->face);

    return atlas;

fail_face:
    free(atlas->bitmap);
    free(atlas->color_bitmap);
    FT_Done_Face(atlas->face);
    FT_Done_FreeType(atlas->ft);
fail:
    if (atlas->fallback_set)
        FcFontSetDestroy(atlas->fallback_set);
    free((void *)atlas->fallback_faces);
    free(atlas);
    return NULL;
}

void ghostcon_atlas_destroy(ghostcon_atlas_t *atlas)
{
    if (!atlas)
        return;
    if (atlas->hb_font)
        hb_font_destroy(atlas->hb_font);
    free(atlas->bitmap);
    free(atlas->color_bitmap);
    if (atlas->fallback_set)
    {
        for (int i = 0; i < atlas->fallback_set->nfont; i++)
        {
            FT_Face f = atlas->fallback_faces[i];
            if (f && f != GHOSTCON_ATLAS_FACE_FAILED)
                FT_Done_Face(f);
        }
        FcFontSetDestroy(atlas->fallback_set);
    }
    free((void *)atlas->fallback_faces);
    FT_Done_Face(atlas->face);
    FT_Done_FreeType(atlas->ft);
    free(atlas);
}

/* Shelf-packs an already-rasterized RGB bitmap (3 bytes/pixel
 *
 * see this file's helpers below for how each FreeType pixel mode gets converted
 * to that shape) into the atlas and fills in `slot->glyph`, sharing the exact
 * packing logic between the font path and the procedural box-drawing path below.
 *
 * `pitch` is the source's stride in bytes (i.e. width*3 for a tightly-packed
 * source). bearing_x/ bearing_y are passed through as-is (see box_draw.c call
 * site for why the procedural path passes bearing_y = atlas->ascent: it makes
 * machine.c's existing "gy = py + ascent -bearing_y" placement formula land the
 * glyph at exactly the cell's top-left corner, with no changes needed to
 * machine.c or gles.c).
 */
static const ghostcon_glyph_t
*pack_bitmap(ghostcon_atlas_t *atlas, atlas_slot_t *slot, uint32_t codepoint, const uint8_t *bitmap, unsigned width, unsigned rows, unsigned pitch, int16_t bearing_x, int16_t bearing_y, float advance)
{
    if (atlas->pen_x + width + GHOSTCON_ATLAS_GLYPH_PADDING > atlas->dim)
    {
        atlas->pen_x = 0;
        atlas->pen_y += atlas->shelf_h + GHOSTCON_ATLAS_GLYPH_PADDING;
        atlas->shelf_h = 0;
    }
    if (atlas->pen_y + rows + GHOSTCON_ATLAS_GLYPH_PADDING > atlas->dim)
        return NULL; /* atlas full */

    for (unsigned row = 0; row < rows; row++)
    {
        uint8_t *dst = atlas->bitmap + (size_t)(atlas->pen_y + row) * atlas->dim * 3
                        + (size_t)atlas->pen_x * 3;
        const uint8_t *src = bitmap + (size_t)row * pitch;
        memcpy(dst, src, (size_t)width * 3);
    }
    atlas->dirty = true;

    slot->occupied = true;
    slot->codepoint = codepoint;
    slot->glyph = (ghostcon_glyph_t){
        .u0 = (float)atlas->pen_x / (float)atlas->dim,
        .v0 = (float)atlas->pen_y / (float)atlas->dim,
        .u1 = (float)(atlas->pen_x + width) / (float)atlas->dim,
        .v1 = (float)(atlas->pen_y + rows) / (float)atlas->dim,
        .width = (int16_t)width,
        .height = (int16_t)rows,
        .bearing_x = bearing_x,
        .bearing_y = bearing_y,
        .advance = advance,
    };

    atlas->pen_x += width + GHOSTCON_ATLAS_GLYPH_PADDING;
    if (rows > atlas->shelf_h)
        atlas->shelf_h = rows;

    return &slot->glyph;
}

/* Same shelf-packing scheme as pack_bitmap() above, into the SEPARATE color
 * atlas/pen state instead
 *
 * see atlas.h's doc comment on ghostcon_glyph_t.is_color for why color glyphs
 * can't share the mask atlas's storage.
 *
 * `pitch` is width*4 for a tightly-packed RGBA source.
 */
static const ghostcon_glyph_t
*pack_bitmap_rgba(ghostcon_atlas_t *atlas, atlas_slot_t *slot, uint32_t codepoint, const uint8_t *bitmap, unsigned width, unsigned rows, unsigned pitch, int16_t bearing_x, int16_t bearing_y, float advance)
{
    if (atlas->color_pen_x + width + GHOSTCON_ATLAS_GLYPH_PADDING > atlas->dim)
    {
        atlas->color_pen_x = 0;
        atlas->color_pen_y += atlas->color_shelf_h + GHOSTCON_ATLAS_GLYPH_PADDING;
        atlas->color_shelf_h = 0;
    }
    if (atlas->color_pen_y + rows + GHOSTCON_ATLAS_GLYPH_PADDING > atlas->dim)
        return NULL; /* color atlas full */

    for (unsigned row = 0; row < rows; row++)
    {
        uint8_t *dst = atlas->color_bitmap + (size_t)(atlas->color_pen_y + row) * atlas->dim * 4
                        + (size_t)atlas->color_pen_x * 4;
        const uint8_t *src = bitmap + (size_t)row * pitch;
        memcpy(dst, src, (size_t)width * 4);
    }
    atlas->color_dirty = true;

    slot->occupied = true;
    slot->codepoint = codepoint;
    slot->glyph = (ghostcon_glyph_t){
        .u0 = (float)atlas->color_pen_x / (float)atlas->dim,
        .v0 = (float)atlas->color_pen_y / (float)atlas->dim,
        .u1 = (float)(atlas->color_pen_x + width) / (float)atlas->dim,
        .v1 = (float)(atlas->color_pen_y + rows) / (float)atlas->dim,
        .width = (int16_t)width,
        .height = (int16_t)rows,
        .bearing_x = bearing_x,
        .bearing_y = bearing_y,
        .advance = advance,
        .is_color = true,
    };

    atlas->color_pen_x += width + GHOSTCON_ATLAS_GLYPH_PADDING;
    if (rows > atlas->color_shelf_h)
        atlas->color_shelf_h = rows;

    return &slot->glyph;
}

/* Picks the fixed bitmap strike (face->available_sizes) whose height is closest
 * to target_px
 *
 * color bitmap fonts (CBDT/CBLC, e.g. Noto Color Emoji) ship a handful of
 * discrete pre-rendered sizes, not a scalable outline, so there's no exact match
 * in general.
 *
 * The chosen strike's actual pixel size (not target_px) becomes the glyph's
 * width/height; machine.c's existing symbol-constraint shrink-to-fit path
 * (ghostcon_cell_is_symbol() now covers emoji
 *
 * see cell.c) already handles the mismatch between whatever strike this picks and
 * the cell's actual size, so no separate scaling logic is needed here.
 */
static int best_fixed_size_index(FT_Face face, int target_px)
{
    int best = 0;
    int best_diff = INT_MAX;
    for (int i = 0; i < face->num_fixed_sizes; i++)
    {
        int diff = abs((int)face->available_sizes[i].height - target_px);
        if (diff < best_diff)
        {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

/* Returns the first face (the primary face, or a lazily-opened fallback from
 * atlas->fallback_set) that actually has a glyph for `codepoint`, and sets
 * *out_gidx to that face's glyph index for it.
 *
 * Returns NULL if none of them do
 *
 * same "give up" outcome as before this fallback chain existed, just checked
 * against more than one font first.
 *
 * Scans the WHOLE fallback list (not stopping at the first hit) because
 * fontconfig's sort order is generic font-similarity, not "best format for this
 * codepoint"
 *
 * found live: for an emoji codepoint, a plain outline font (e.g.
 *
 * DejaVu Sans, which has a monochrome fallback glyph for a lot of emoji) commonly
 * ranks ahead of a real color-bitmap emoji font (e.g.
 *
 * Noto Color Emoji) in that order, so stopping at the first match would silently
 * prefer the worse-looking glyph every time.
 *
 * A color-capable face's glyph wins immediately when found; otherwise the first
 * (in fontconfig's order) ordinary match is used, same as before this preference
 * existed.
 *
 * The full scan only costs anything on a genuine cache miss
 *
 * every candidate face opened here is cached in fallback_faces[], so a repeat
 * lookup for a different codepoint from the same fallback set doesn't reopen
 * anything.
 */
static FT_Face
find_face_for_codepoint(ghostcon_atlas_t *atlas, uint32_t codepoint, FT_UInt *out_gidx)
{
    FT_UInt gidx = FT_Get_Char_Index(atlas->face, codepoint);
    if (gidx != 0)
    {
        *out_gidx = gidx;
        return atlas->face;
    }

    if (!atlas->fallback_set)
        return NULL;

    FT_Face first_match = NULL;
    FT_UInt first_match_gidx = 0;

    for (int i = 0; i < atlas->fallback_set->nfont; i++)
    {
        FT_Face face = atlas->fallback_faces[i];
        if (face == GHOSTCON_ATLAS_FACE_FAILED)
            continue;
        if (!face)
        {
            FcChar8 *path = NULL;
            if (FcPatternGetString(atlas->fallback_set->fonts[i], FC_FILE, 0, &path) != FcResultMatch ||
                FT_New_Face(atlas->ft, (const char *)path, 0, &face))
            {
                atlas->fallback_faces[i] = GHOSTCON_ATLAS_FACE_FAILED;
                continue;
            }
            FT_Set_Pixel_Sizes(face, 0, (FT_UInt)atlas->font_size_px);
            atlas->fallback_faces[i] = face;
        }
        FT_UInt g = FT_Get_Char_Index(face, codepoint);
        if (g == 0)
            continue;
        if (FT_HAS_COLOR(face))
        {
            *out_gidx = g;
            return face;
        }
        if (!first_match)
        {
            first_match = face;
            first_match_gidx = g;
        }
    }
    if (first_match)
    {
        *out_gidx = first_match_gidx;
        return first_match;
    }
    return NULL;
}

static const ghostcon_glyph_t *load_glyph_into_slot(ghostcon_atlas_t *atlas, atlas_slot_t *slot, uint32_t slot_key, FT_Face face, FT_UInt gidx);

const ghostcon_glyph_t
*ghostcon_atlas_glyph(ghostcon_atlas_t *atlas, uint32_t codepoint)
{
    bool found;
    atlas_slot_t *slot = slot_find(atlas, codepoint, &found);
    if (!slot)
        return NULL; /* table full */
    if (found)
        return &slot->glyph;

    /* Box-drawing / block-element / legacy-computing-sextant characters are meant
     * to tile edge-to-edge across adjacent cells
     *
     * sourcing them from the font, like every other glyph, leaves whatever
     * bearing/padding that font happens to use around them, which is invisible
     * for ordinary letters but a visible seam for shapes meant to connect
     * seamlessly (found live: a TUI splash logo built from these characters
     * rendered as a visible checkerboard of gaps).
     *
     * See box_draw.h's own doc comment for the full reasoning and PLAN.md for the
     * live repro this fixes.
     */
    /* Cell dimensions come from font metrics at a fixed, small pixel size (see
     * core/main.c's FONT_SIZE)
     *
     * 128x128 is generous headroom, not a real limit.
     */
    if (atlas->cell_w > 0 && atlas->cell_h > 0 &&
        atlas->cell_w <= 128 && atlas->cell_h <= 128)
    {
        uint8_t box_bitmap[128 * 128];
        if (ghostcon_box_draw_render(codepoint, atlas->cell_w, atlas->cell_h, box_bitmap))
        {
            /* Procedural glyphs are a plain mask
             *
             * replicate into RGB (R=G=B), never real per-subpixel data.
             */
            uint8_t conv[128 * 128 * 3];
            for (int row = 0; row < atlas->cell_h; row++)
            {
                const uint8_t *src = box_bitmap + row * atlas->cell_w;
                uint8_t *dst = conv + (size_t)row * atlas->cell_w * 3;
                for (int col = 0; col < atlas->cell_w; col++)
                {
                    dst[col * 3 + 0] = dst[col * 3 + 1] = dst[col * 3 + 2] = src[col];
                }
            }
            return pack_bitmap(atlas, slot, codepoint, conv,
                                (unsigned)atlas->cell_w, (unsigned)atlas->cell_h,
                                (unsigned)atlas->cell_w * 3,
                                0, (int16_t)atlas->ascent, (float)atlas->cell_w);
        }
    }

    FT_UInt gidx;
    FT_Face face = find_face_for_codepoint(atlas, codepoint, &gidx);
    if (!face)
        return NULL;

    return load_glyph_into_slot(atlas, slot, codepoint, face, gidx);
}

/* Shared by ghostcon_atlas_glyph() (codepoint lookup, any face in the fallback
 * chain) and ghostcon_atlas_glyph_by_index() (raw glyph index in the primary
 * face, for HarfBuzz-shaped ligatures
 *
 * see atlas.h).
 *
 * `slot_key` is whatever slot_find() was called with (a real codepoint for the
 * former, a synthetic 0x80000000|glyph_index for the latter) purely for
 * atlas_slot_t bookkeeping
 *
 * rendering itself only cares about face+gidx.
 */
static const ghostcon_glyph_t
*load_glyph_into_slot(ghostcon_atlas_t *atlas, atlas_slot_t *slot, uint32_t slot_key, FT_Face face, FT_UInt gidx)
{
    /* Color bitmap fonts (CBDT/CBLC, e.g. Noto Color Emoji) need a completely
     * different load path from ordinary outline fonts: FT_LOAD_COLOR (so FreeType
     * hands back the color strike instead of trying
     *
     * and failing
     *
     * to render an outline that doesn't exist for this face), and FT_Select_Size
     * to pick which discrete strike size, since these fonts aren't scalable.
     *
     * See best_fixed_size_index()'s doc comment above for why an exact size match
     * isn't expected or needed.
     */
    if (FT_HAS_COLOR(face))
    {
        FT_Select_Size(face, best_fixed_size_index(face, atlas->font_size_px));
        if (FT_Load_Glyph(face, gidx, FT_LOAD_COLOR | FT_LOAD_RENDER))
            return NULL;

        FT_GlyphSlot cg = face->glyph;
        FT_Bitmap *cbmp = &cg->bitmap;
        if (cbmp->pixel_mode != FT_PIXEL_MODE_BGRA)
            return NULL; /* unexpected for a FT_HAS_COLOR face -- give up gracefully */
        /* Wider cap than the mono/outline path's 128x128 (see below) --color
         * bitmap strikes are fixed, pre-rendered sizes, not scaled to the target
         * pixel size, and commonly come out larger than a small text font size
         * would suggest: found live, Noto Color Emoji's closest strike to this
         * project's actual font_size_px is 136x128, already past 128 wide.
         */
        if (cbmp->width == 0 || cbmp->width > GHOSTCON_ATLAS_COLOR_GLYPH_MAX ||
            cbmp->rows > GHOSTCON_ATLAS_COLOR_GLYPH_MAX)
            return NULL;

        /* FreeType's color bitmaps are BGRA, premultiplied alpha. gles.c's
         * color-glyph shader path samples this atlas as direct
         * (non-premultiplied) source color, so both the channel order and the
         * premultiplication need undoing here, once, at pack time rather than
         * per-frame in the shader.
         */
        uint8_t conv[GHOSTCON_ATLAS_COLOR_GLYPH_MAX * GHOSTCON_ATLAS_COLOR_GLYPH_MAX * 4];
        for (unsigned row = 0; row < cbmp->rows; row++)
        {
            const uint8_t *src = cbmp->buffer + (size_t)row * (unsigned)cbmp->pitch;
            uint8_t *dst = conv + (size_t)row * cbmp->width * 4;
            for (unsigned col = 0; col < cbmp->width; col++)
            {
                uint8_t bch = src[col * 4 + 0], gch = src[col * 4 + 1],
                        rch = src[col * 4 + 2], ach = src[col * 4 + 3];
                if (ach > 0 && ach < 255)
                {
                    rch = (uint8_t)((unsigned)rch * 255 / ach);
                    gch = (uint8_t)((unsigned)gch * 255 / ach);
                    bch = (uint8_t)((unsigned)bch * 255 / ach);
                }
                dst[col * 4 + 0] = rch;
                dst[col * 4 + 1] = gch;
                dst[col * 4 + 2] = bch;
                dst[col * 4 + 3] = ach;
            }
        }
        return pack_bitmap_rgba(atlas, slot, slot_key, conv, cbmp->width, cbmp->rows,
                                 cbmp->width * 4, (int16_t)cg->bitmap_left, (int16_t)cg->bitmap_top,
                                 (float)(cg->advance.x >> 6));
    }

    FT_Int32 load_flags = FT_LOAD_RENDER;
    switch (atlas->aa_mode)
    {
    case GC_ATLAS_AA_NONE:                          load_flags |= FT_LOAD_TARGET_MONO; break;
    case GC_ATLAS_AA_SUBPIXEL: case GC_ATLAS_AA_CLEARTYPE: load_flags |= FT_LOAD_TARGET_LCD; break;
    default:                                        load_flags |= FT_LOAD_TARGET_NORMAL; break;
    }
    if (FT_Load_Glyph(face, gidx, load_flags))
        return NULL;

    FT_GlyphSlot g = face->glyph;
    FT_Bitmap *bmp = &g->bitmap;

    /* pack_bitmap() expects 3 bytes (RGB) per pixel
     *
     * every FreeType pixel mode needs converting to that shape: MONO is
     * bit-packed (1 bit/pixel), GRAY is already single-byte coverage but needs
     * replicating across channels, and LCD is 3-byte RGB triplets/ pixel (width
     * reported as 3x the real glyph width) that either get averaged down to a
     * replicated mask (GC_ATLAS_AA_SUBPIXEL --LCD-optimized hinting only, no real
     * subpixel color) or copied through as genuine per-subpixel coverage,
     * respecting the configured physical subpixel order (GC_ATLAS_AA_CLEARTYPE).
     *
     * Bounded scratch buffer matches the box-drawing path's own 128x128 headroom
     * precedent above
     *
     * an absurdly large glyph at any sane font size just silently doesn't render,
     * same "give up gracefully" convention as an atlas-full/no-outline miss
     * elsewhere in this function.
     *
     * A genuinely EMPTY bitmap (real_width==0 and/or bmp->rows==0) is NOT a
     * failure to reject here, unlike the too-large case
     *
     * found live: some fonts' ligature substitutions (see ligature.h) use a real,
     * deliberately empty glyph for one half of a 2-glyph pair (JetBrains Mono
     * does this for every pair this project tested).
     *
     * Rejecting it as if the load had failed sent ghostcon_atlas_glyph_by_index()
     * down the "no glyph" path, which render/machine.c was (reasonably) treating
     * as "fall back to the plain per-codepoint glyph"
     *
     * drawing a real, visible character where the font intended nothing to appear
     * at all (a stray disconnected mark next to the real ligature glyph, e.g.
     * "!="'s "!" leaving a tiny tick above the "≠"). pack_bitmap() below already
     * handles width==0/rows==0 safely (copies nothing, packs a zero-size UV
     * rect), so just let it through.
     */
    unsigned real_width = (bmp->pixel_mode == FT_PIXEL_MODE_LCD) ? bmp->width / 3 : bmp->width;
    if (real_width > 128 || bmp->rows > 128)
        return NULL;
    uint8_t conv[128 * 128 * 3];
    for (unsigned row = 0; row < bmp->rows; row++)
    {
        const uint8_t *src = bmp->buffer + (size_t)row * (unsigned)bmp->pitch;
        uint8_t *dst = conv + (size_t)row * real_width * 3;
        switch (bmp->pixel_mode)
        {
        case FT_PIXEL_MODE_MONO:
            for (unsigned col = 0; col < real_width; col++)
            {
                uint8_t v = (src[col / 8] & (0x80 >> (col % 8))) ? 0xFF : 0x00;
                dst[col * 3 + 0] = dst[col * 3 + 1] = dst[col * 3 + 2] = v;
            }
            break;
        case FT_PIXEL_MODE_LCD:
            if (atlas->aa_mode == GC_ATLAS_AA_CLEARTYPE)
            {
                /* Real per-subpixel coverage.
                 *
                 * FreeType always writes bytes in physical left-to-right order;
                 * map that to R/G/B according to the panel's actual subpixel
                 * order
                 *
                 * getting this backwards produces visible color fringing instead
                 * of removing it.
                 */
                for (unsigned col = 0; col < real_width; col++)
                {
                    uint8_t b0 = src[col * 3 + 0], b1 = src[col * 3 + 1], b2 = src[col * 3 + 2];
                    if (atlas->bgr_order)
                    {
                        dst[col * 3 + 0] = b2;
                        dst[col * 3 + 1] = b1;
                        dst[col * 3 + 2] = b0;
                    }
                    else
                    {
                        dst[col * 3 + 0] = b0;
                        dst[col * 3 + 1] = b1;
                        dst[col * 3 + 2] = b2;
                    }
                }
            }
            else
            { /* GC_ATLAS_AA_SUBPIXEL -- average into a replicated mask */
                for (unsigned col = 0; col < real_width; col++)
                {
                    uint8_t v = (uint8_t)(((unsigned)src[col * 3] + src[col * 3 + 1] + src[col * 3 + 2]) / 3);
                    dst[col * 3 + 0] = dst[col * 3 + 1] = dst[col * 3 + 2] = v;
                }
            }
            break;
        default: /* FT_PIXEL_MODE_GRAY */
            for (unsigned col = 0; col < real_width; col++)
            {
                uint8_t v = src[col];
                dst[col * 3 + 0] = dst[col * 3 + 1] = dst[col * 3 + 2] = v;
            }
            break;
        }
    }
    return pack_bitmap(atlas, slot, slot_key, conv, real_width, bmp->rows, real_width * 3,
                        (int16_t)g->bitmap_left, (int16_t)g->bitmap_top, (float)(g->advance.x >> 6));
}

/* Synthetic slot key for ghostcon_atlas_glyph_by_index()
 *
 * real Unicode codepoints top out at 0x10FFFF, well clear of the high bit, so
 * this can never collide with a genuine codepoint's slot.
 */
#define GHOSTCON_ATLAS_GIDX_SLOT_KEY(gidx) (0x80000000u | (uint32_t)(gidx))

const ghostcon_glyph_t
*ghostcon_atlas_glyph_by_index(ghostcon_atlas_t *atlas, uint32_t glyph_index)
{
    uint32_t key = GHOSTCON_ATLAS_GIDX_SLOT_KEY(glyph_index);
    bool found;
    atlas_slot_t *slot = slot_find(atlas, key, &found);
    if (!slot)
        return NULL; /* table full */
    if (found)
        return &slot->glyph;
    return load_glyph_into_slot(atlas, slot, key, atlas->face, (FT_UInt)glyph_index);
}

int ghostcon_atlas_shape_run(ghostcon_atlas_t *atlas, const uint32_t *codepoints, int count, uint32_t *out_glyph_indices, float *out_advances, int max_out)
{
    if (!atlas->hb_font)
        return -1;

    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf32(buf, codepoints, count, 0, count);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    /* Force 'latn', not HB_SCRIPT_COMMON's 'DFLT'
     *
     * most fonts (including the JetBrains Mono family, confirmed live) register
     * their calt/liga ligature lookups only under the Latin script table, not
     * DFLT, so shaping punctuation as script-neutral silently finds no ligature
     * feature at all even when the font genuinely has one.
     */
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_get_default());
    hb_buffer_guess_segment_properties(buf);

    hb_shape(atlas->hb_font, buf, NULL, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buf, &glyph_count);

    int n = (int)glyph_count;
    if (n > max_out)
        n = max_out;
    for (int i = 0; i < n; i++)
    {
        out_glyph_indices[i] = infos[i].codepoint; /* HarfBuzz repurposes this field as glyph index post-shaping */
        out_advances[i] = (float)(positions[i].x_advance) / 64.0f;
    }

    hb_buffer_destroy(buf);
    return (int)glyph_count;
}

uint32_t ghostcon_atlas_plain_glyph_index(ghostcon_atlas_t *atlas, uint32_t codepoint)
{
    return (uint32_t)FT_Get_Char_Index(atlas->face, codepoint);
}

const uint8_t *ghostcon_atlas_bitmap(const ghostcon_atlas_t *atlas)
{
    return atlas->bitmap;
}

uint32_t ghostcon_atlas_dim(const ghostcon_atlas_t *atlas)
{
    return atlas->dim;
}

bool ghostcon_atlas_dirty(const ghostcon_atlas_t *atlas)
{
    return atlas->dirty;
}

void ghostcon_atlas_clear_dirty(ghostcon_atlas_t *atlas)
{
    atlas->dirty = false;
}

const uint8_t *ghostcon_atlas_color_bitmap(const ghostcon_atlas_t *atlas)
{
    return atlas->color_bitmap;
}

bool ghostcon_atlas_color_dirty(const ghostcon_atlas_t *atlas)
{
    return atlas->color_dirty;
}

void ghostcon_atlas_color_clear_dirty(ghostcon_atlas_t *atlas)
{
    atlas->color_dirty = false;
}

void ghostcon_atlas_cell_size(const ghostcon_atlas_t *atlas, int *cell_w, int *cell_h)
{
    *cell_w = atlas->cell_w;
    *cell_h = atlas->cell_h;
}

int ghostcon_atlas_ascent(const ghostcon_atlas_t *atlas)
{
    return atlas->ascent;
}
