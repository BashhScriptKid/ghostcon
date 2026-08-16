#include "ghostcon/render/atlas.h"
#include "ghostcon/render/box_draw.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdlib.h>
#include <string.h>

/* Fixed-size open-addressing hash table. Terminal glyph sets are small
   (ASCII + a modest working set of Unicode) — no need for a resizable
   map. GHOSTCON_ATLAS_MAX_GLYPHS is generous headroom, not a hard cap
   on what any one session can display (a full atlas just means new
   glyphs stop being cached and lose the ability to render, which is
   the same failure mode as the atlas bitmap itself filling up). */
#define GHOSTCON_ATLAS_MAX_GLYPHS 4096

typedef struct {
    uint32_t codepoint; /* 0 = empty slot (codepoint 0 / NUL is never rendered) */
    bool     occupied;
    ghostcon_glyph_t glyph;
} atlas_slot_t;

struct ghostcon_atlas {
    FT_Library ft;
    FT_Face    face;

    uint32_t dim;
    uint8_t *bitmap; /* dim * dim, one byte (alpha) per pixel */
    bool     dirty;

    /* Shelf packer state */
    uint32_t pen_x, pen_y, shelf_h;

    int cell_w, cell_h;
    int ascent; /* baseline offset from cell top, pixels */

    atlas_slot_t slots[GHOSTCON_ATLAS_MAX_GLYPHS];
};

static atlas_slot_t *
slot_find(ghostcon_atlas_t *atlas, uint32_t codepoint, bool *found)
{
    uint32_t h = codepoint * 2654435761u; /* Knuth multiplicative hash */
    for (uint32_t i = 0; i < GHOSTCON_ATLAS_MAX_GLYPHS; i++) {
        uint32_t idx = (h + i) % GHOSTCON_ATLAS_MAX_GLYPHS;
        atlas_slot_t *s = &atlas->slots[idx];
        if (!s->occupied) {
            *found = false;
            return s;
        }
        if (s->codepoint == codepoint) {
            *found = true;
            return s;
        }
    }
    *found = false;
    return NULL; /* table full */
}

ghostcon_atlas_t *
ghostcon_atlas_create(const char *font_family, int font_size_px, uint32_t atlas_dim)
{
    ghostcon_atlas_t *atlas = calloc(1, sizeof(*atlas));
    if (!atlas)
        return NULL;

    if (!FcInit())
        goto fail;

    FcPattern *pat = FcPatternCreate();
    if (font_family)
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)font_family);
    FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    if (!match)
        goto fail;

    FcChar8 *file_path = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file_path) != FcResultMatch) {
        FcPatternDestroy(match);
        goto fail;
    }

    if (FT_Init_FreeType(&atlas->ft)) {
        FcPatternDestroy(match);
        goto fail;
    }
    if (FT_New_Face(atlas->ft, (const char *)file_path, 0, &atlas->face)) {
        FcPatternDestroy(match);
        goto fail;
    }
    FcPatternDestroy(match);

    FT_Set_Pixel_Sizes(atlas->face, 0, (FT_UInt)font_size_px);

    /* Monospace cell size: advance of a representative ASCII glyph, and
       the face's line height. */
    if (FT_Load_Char(atlas->face, 'M', FT_LOAD_DEFAULT) == 0)
        atlas->cell_w = (int)(atlas->face->glyph->advance.x >> 6);
    if (atlas->cell_w <= 0)
        atlas->cell_w = font_size_px / 2 + 1;
    atlas->cell_h = (int)(atlas->face->size->metrics.height >> 6);
    if (atlas->cell_h <= 0)
        atlas->cell_h = font_size_px + 2;

    /* Baseline offset from the cell's top edge. Must come from the
       font's line metrics (ascender), NOT from any per-glyph bearing —
       anchoring per-glyph would put descenders (g, q, y, p, j) at a
       different baseline than the rest of the line and, worse, using
       cell_h itself as the anchor (as an earlier version of this code
       did) leaves zero room below the baseline, so descenders spill
       into the next row and get overdrawn by its background quad. */
    atlas->ascent = (int)(atlas->face->size->metrics.ascender >> 6);
    if (atlas->ascent <= 0)
        atlas->ascent = atlas->cell_h * 4 / 5; /* rough fallback */

    atlas->dim = atlas_dim;
    atlas->bitmap = calloc(1, (size_t)atlas_dim * atlas_dim);
    if (!atlas->bitmap)
        goto fail_face;

    /* Reserve a 2x2 opaque block at the atlas origin for solid-color
       quads (background cells, cursor) — see ghostcon_atlas_reserved_uv()
       and render/gles.c's push_rect. Advance the shelf packer past it so
       no real glyph ever lands there. */
    atlas->bitmap[0] = 0xFF;
    atlas->bitmap[1] = 0xFF;
    atlas->bitmap[atlas_dim] = 0xFF;
    atlas->bitmap[atlas_dim + 1] = 0xFF;
    atlas->pen_x = 2;
    atlas->shelf_h = 2;

    atlas->dirty = true;

    return atlas;

fail_face:
    FT_Done_Face(atlas->face);
    FT_Done_FreeType(atlas->ft);
fail:
    free(atlas);
    return NULL;
}

void
ghostcon_atlas_destroy(ghostcon_atlas_t *atlas)
{
    if (!atlas)
        return;
    free(atlas->bitmap);
    FT_Done_Face(atlas->face);
    FT_Done_FreeType(atlas->ft);
    free(atlas);
}

/* Shelf-packs an already-rasterized bitmap into the atlas and fills in
   `slot->glyph`, sharing the exact packing logic between the font path
   and the procedural box-drawing path below. bearing_x/bearing_y are
   passed through as-is (see box_draw.c call site for why the
   procedural path passes bearing_y = atlas->ascent: it makes
   machine.c's existing "gy = py + ascent - bearing_y" placement
   formula land the glyph at exactly the cell's top-left corner, with
   no changes needed to machine.c or gles.c). */
static const ghostcon_glyph_t *
pack_bitmap(ghostcon_atlas_t *atlas, atlas_slot_t *slot, uint32_t codepoint,
            const uint8_t *bitmap, unsigned width, unsigned rows,
            unsigned pitch, int16_t bearing_x, int16_t bearing_y, float advance)
{
    if (atlas->pen_x + width > atlas->dim) {
        atlas->pen_x = 0;
        atlas->pen_y += atlas->shelf_h;
        atlas->shelf_h = 0;
    }
    if (atlas->pen_y + rows > atlas->dim)
        return NULL; /* atlas full */

    for (unsigned row = 0; row < rows; row++) {
        uint8_t *dst = atlas->bitmap + (atlas->pen_y + row) * atlas->dim + atlas->pen_x;
        const uint8_t *src = bitmap + row * pitch;
        memcpy(dst, src, width);
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

    atlas->pen_x += width;
    if (rows > atlas->shelf_h)
        atlas->shelf_h = rows;

    return &slot->glyph;
}

const ghostcon_glyph_t *
ghostcon_atlas_glyph(ghostcon_atlas_t *atlas, uint32_t codepoint)
{
    bool found;
    atlas_slot_t *slot = slot_find(atlas, codepoint, &found);
    if (!slot)
        return NULL; /* table full */
    if (found)
        return &slot->glyph;

    /* Box-drawing / block-element / legacy-computing-sextant characters
       are meant to tile edge-to-edge across adjacent cells -- sourcing
       them from the font, like every other glyph, leaves whatever
       bearing/padding that font happens to use around them, which is
       invisible for ordinary letters but a visible seam for shapes
       meant to connect seamlessly (found live: a TUI splash logo built
       from these characters rendered as a visible checkerboard of
       gaps). See box_draw.h's own doc comment for the full reasoning
       and PLAN.md for the live repro this fixes. */
    /* Cell dimensions come from font metrics at a fixed, small pixel
       size (see core/main.c's FONT_SIZE) -- 128x128 is generous
       headroom, not a real limit. */
    if (atlas->cell_w > 0 && atlas->cell_h > 0 &&
        atlas->cell_w <= 128 && atlas->cell_h <= 128) {
        uint8_t box_bitmap[128 * 128];
        if (ghostcon_box_draw_render(codepoint, atlas->cell_w, atlas->cell_h, box_bitmap)) {
            return pack_bitmap(atlas, slot, codepoint, box_bitmap,
                                (unsigned)atlas->cell_w, (unsigned)atlas->cell_h,
                                (unsigned)atlas->cell_w,
                                0, (int16_t)atlas->ascent, (float)atlas->cell_w);
        }
    }

    FT_UInt gidx = FT_Get_Char_Index(atlas->face, codepoint);
    if (gidx == 0)
        return NULL;
    if (FT_Load_Glyph(atlas->face, gidx, FT_LOAD_RENDER))
        return NULL;

    FT_GlyphSlot g = atlas->face->glyph;
    FT_Bitmap *bmp = &g->bitmap;

    return pack_bitmap(atlas, slot, codepoint, bmp->buffer, bmp->width, bmp->rows,
                        (unsigned)bmp->pitch, (int16_t)g->bitmap_left, (int16_t)g->bitmap_top,
                        (float)(g->advance.x >> 6));
}

const uint8_t *
ghostcon_atlas_bitmap(const ghostcon_atlas_t *atlas)
{
    return atlas->bitmap;
}

uint32_t
ghostcon_atlas_dim(const ghostcon_atlas_t *atlas)
{
    return atlas->dim;
}

bool
ghostcon_atlas_dirty(const ghostcon_atlas_t *atlas)
{
    return atlas->dirty;
}

void
ghostcon_atlas_clear_dirty(ghostcon_atlas_t *atlas)
{
    atlas->dirty = false;
}

void
ghostcon_atlas_cell_size(const ghostcon_atlas_t *atlas, int *cell_w, int *cell_h)
{
    *cell_w = atlas->cell_w;
    *cell_h = atlas->cell_h;
}

int
ghostcon_atlas_ascent(const ghostcon_atlas_t *atlas)
{
    return atlas->ascent;
}
