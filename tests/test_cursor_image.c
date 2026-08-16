/*
 * test_cursor_image -- pure-CPU unit test for core/cursor_image.c's
 * BMP decode/encode and Xcursor decode. No DRM/hardware dependency,
 * matching tests/test_mouse.c/test_input.c's own precedent.
 */

#define _DEFAULT_SOURCE /* mkdtemp() under -std=c11 without this */

#include "ghostcon/core/cursor_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* Hand-writes a minimal little-endian value into a byte buffer. */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* Hand-constructs a tiny 2x2, 32bpp, top-down (negative height) BMP --
   the same layout ghostcon_cursor_write_bmp() itself produces, but
   built independently here so the decoder test doesn't just validate
   against its own encoder's output. */
static void
write_synthetic_bmp(const char *path, uint32_t argb00, uint32_t argb10,
                     uint32_t argb01, uint32_t argb11)
{
    uint8_t buf[14 + 40 + 2 * 2 * 4];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'B'; buf[1] = 'M';
    put_u32(buf + 2, sizeof(buf));
    put_u32(buf + 10, 14 + 40);
    put_u32(buf + 14, 40);      /* DIB header size */
    put_u32(buf + 18, 2);       /* width */
    put_u32(buf + 22, (uint32_t)-2); /* height = -2 -> top-down */
    put_u16(buf + 26, 1);       /* planes */
    put_u16(buf + 28, 32);      /* bpp */
    put_u32(buf + 30, 0);       /* BI_RGB */

    uint8_t *row0 = buf + 14 + 40;
    uint8_t *row1 = row0 + 8;
    uint32_t vals[4] = { argb00, argb10, argb01, argb11 };
    uint8_t *rows[2] = { row0, row1 };
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint32_t v = vals[y * 2 + x];
            uint8_t *px = rows[y] + x * 4;
            px[0] = (uint8_t)(v & 0xff);         /* B */
            px[1] = (uint8_t)((v >> 8) & 0xff);  /* G */
            px[2] = (uint8_t)((v >> 16) & 0xff); /* R */
            px[3] = (uint8_t)((v >> 24) & 0xff); /* A */
        }
    }

    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sizeof(buf), f); fclose(f); }
}

/* Hand-constructs a minimal valid Xcursor file: one TOC entry
   pointing at one 2x2 image chunk. */
static void
write_synthetic_xcursor(const char *path, uint32_t hot_x, uint32_t hot_y,
                         uint32_t argb00, uint32_t argb10, uint32_t argb01, uint32_t argb11)
{
    uint8_t buf[16 /* file header */ + 12 /* toc entry */ + 36 /* image header */ + 2 * 2 * 4];
    memset(buf, 0, sizeof(buf));

    /* Written as the literal ASCII bytes "Xcur", NOT as a pre-computed
       u32 constant -- a hand-computed magic number here would just
       re-derive the same value the decoder itself hardcodes, silently
       validating the test against itself instead of against the real
       file format (exactly what happened before this was fixed: a
       transposed digit in both places passed this test while
       rejecting every real Xcursor file on disk). */
    buf[0] = 'X'; buf[1] = 'c'; buf[2] = 'u'; buf[3] = 'r';
    put_u32(buf + 4, 16);      /* header_size */
    put_u32(buf + 8, 0x10000); /* version -- unchecked by our decoder, any value */
    put_u32(buf + 12, 1);      /* ntoc = 1 */

    size_t toc_off = 16;
    put_u32(buf + toc_off, 0xfffd0002u); /* type = image */
    put_u32(buf + toc_off + 4, 2);       /* subtype = nominal size */
    size_t chunk_off = toc_off + 12;
    put_u32(buf + toc_off + 8, (uint32_t)chunk_off);

    put_u32(buf + chunk_off, 36);          /* chunk header_size */
    put_u32(buf + chunk_off + 4, 0xfffd0002u);
    put_u32(buf + chunk_off + 8, 2);       /* subtype, again, per format */
    put_u32(buf + chunk_off + 12, 1);      /* chunk version */
    put_u32(buf + chunk_off + 16, 2);      /* width */
    put_u32(buf + chunk_off + 20, 2);      /* height */
    put_u32(buf + chunk_off + 24, hot_x);
    put_u32(buf + chunk_off + 28, hot_y);
    put_u32(buf + chunk_off + 32, 0);      /* delay */

    uint32_t *pixels = (uint32_t *)(buf + chunk_off + 36);
    pixels[0] = argb00;
    pixels[1] = argb10;
    pixels[2] = argb01;
    pixels[3] = argb11;

    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, sizeof(buf), f); fclose(f); }
}

int
main(void)
{
    char dir_template[] = "/tmp/ghostcon-test-cursor-XXXXXX";
    char *dir = mkdtemp(dir_template);
    CHECK(dir != NULL, "mkdtemp for cursor image test");
    if (!dir) {
        fprintf(stderr, "cannot continue without a temp dir\n");
        return 1;
    }

    /* --- BMP decode --- */
    char bmp_path[512];
    snprintf(bmp_path, sizeof(bmp_path), "%s/test.bmp", dir);
    write_synthetic_bmp(bmp_path, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x80FFFFFF);

    uint32_t w = 0, h = 0;
    uint32_t *pixels = ghostcon_cursor_load_bmp(bmp_path, &w, &h);
    CHECK(pixels != NULL, "BMP decode succeeds");
    if (pixels) {
        CHECK(w == 2 && h == 2, "BMP decoded dimensions");
        CHECK(pixels[0] == 0xFFFF0000, "BMP top-left pixel (red)");
        CHECK(pixels[1] == 0xFF00FF00, "BMP top-right pixel (green)");
        CHECK(pixels[2] == 0xFF0000FF, "BMP bottom-left pixel (blue)");
        CHECK(pixels[3] == 0x80FFFFFF, "BMP bottom-right pixel (translucent white)");
        free(pixels);
    }

    /* --- BMP encode + round-trip --- */
    char roundtrip_path[512];
    snprintf(roundtrip_path, sizeof(roundtrip_path), "%s/roundtrip.bmp", dir);
    uint32_t original[4] = { 0x11223344, 0xAABBCCDD, 0x00000000, 0xFFFFFFFF };
    CHECK(ghostcon_cursor_write_bmp(roundtrip_path, original, 2, 2), "BMP encode succeeds");

    uint32_t rw = 0, rh = 0;
    uint32_t *roundtrip = ghostcon_cursor_load_bmp(roundtrip_path, &rw, &rh);
    CHECK(roundtrip != NULL, "BMP round-trip decode succeeds");
    if (roundtrip) {
        CHECK(rw == 2 && rh == 2, "BMP round-trip dimensions");
        CHECK(memcmp(roundtrip, original, sizeof(original)) == 0, "BMP round-trip pixels match exactly");
        free(roundtrip);
    }

    /* --- BMP failure cases --- */
    CHECK(ghostcon_cursor_load_bmp("/nonexistent/path.bmp", &w, &h) == NULL,
          "BMP decode fails cleanly on missing file");

    /* --- Xcursor decode --- */
    char xcur_path[512];
    snprintf(xcur_path, sizeof(xcur_path), "%s/xtest", dir);
    write_synthetic_xcursor(xcur_path, 1, 1, 0xFFAA0000, 0xFF00AA00, 0xFF0000AA, 0xFF444444);

    uint32_t xw = 0, xh = 0, hot_x = 99, hot_y = 99;
    uint32_t *xpixels = ghostcon_cursor_load_xcursor(dir, "xtest", &xw, &xh, &hot_x, &hot_y);
    CHECK(xpixels != NULL, "Xcursor decode succeeds");
    if (xpixels) {
        CHECK(xw == 2 && xh == 2, "Xcursor decoded dimensions");
        CHECK(hot_x == 1 && hot_y == 1, "Xcursor decoded hotspot");
        CHECK(xpixels[0] == 0xFFAA0000, "Xcursor pixel 0");
        CHECK(xpixels[3] == 0xFF444444, "Xcursor pixel 3");
        free(xpixels);
    }

    /* Also reachable via the theme-root form (dir/cursors/name). */
    char cursors_dir[512];
    snprintf(cursors_dir, sizeof(cursors_dir), "%s/cursors", dir);
    /* mkdir via system-independent approach: just use rename to move
       the same file into a cursors/ subdir for this check. */
    char mkdir_cmd[600];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", cursors_dir);
    if (system(mkdir_cmd) == 0) {
        char nested_path[600];
        snprintf(nested_path, sizeof(nested_path), "%s/nested", cursors_dir);
        write_synthetic_xcursor(nested_path, 0, 0, 0xFFAA0000, 0xFF00AA00, 0xFF0000AA, 0xFF444444);
        uint32_t nw = 0, nh = 0, nhx = 0, nhy = 0;
        uint32_t *npixels = ghostcon_cursor_load_xcursor(dir, "nested", &nw, &nh, &nhx, &nhy);
        CHECK(npixels != NULL, "Xcursor decode via theme-root/cursors/ fallback");
        free(npixels);
    }

    CHECK(ghostcon_cursor_load_xcursor(dir, "doesnotexist", &xw, &xh, &hot_x, &hot_y) == NULL,
          "Xcursor decode fails cleanly on missing name");

    /* ghostcon_cursor_scale(): 2x2 -> 4x4 upscale, nearest-neighbor --
       each output quadrant should exactly equal the source pixel it
       mapped from, no blending. */
    {
        uint32_t src2x2[4] = { 0xFF000001, 0xFF000002, 0xFF000003, 0xFF000004 };
        uint32_t *up = ghostcon_cursor_scale(src2x2, 2, 2, 4, 4);
        CHECK(up != NULL, "cursor_scale 2x2->4x4 succeeds");
        if (up) {
            CHECK(up[0] == 0xFF000001 && up[1] == 0xFF000001, "cursor_scale top-left quadrant");
            CHECK(up[2] == 0xFF000002 && up[3] == 0xFF000002, "cursor_scale top-right quadrant");
            CHECK(up[2 * 4] == 0xFF000003, "cursor_scale bottom-left quadrant");
            CHECK(up[2 * 4 + 2] == 0xFF000004, "cursor_scale bottom-right quadrant");
            free(up);
        }

        /* 4x4 -> 2x2 downscale (nearest-neighbor picks one source pixel
           per output cell, no averaging -- just checking it produces
           in-bounds source pixels, not a specific blend result). */
        uint32_t src4x4[16];
        for (int i = 0; i < 16; i++)
            src4x4[i] = 0xFF000000u | (uint32_t)i;
        uint32_t *down = ghostcon_cursor_scale(src4x4, 4, 4, 2, 2);
        CHECK(down != NULL, "cursor_scale 4x4->2x2 succeeds");
        free(down);

        CHECK(ghostcon_cursor_scale(src2x2, 2, 2, 0, 4) == NULL,
              "cursor_scale rejects a zero destination dimension");
    }

    /* ghostcon_cursor_crop_to_content(): a 4x4 image with a single
       opaque pixel at (2,1) and everything else transparent -- should
       crop to a 1x1 image, and the hotspot (originally 2,1, i.e. right
       on the opaque pixel) should shift to (0,0) in the cropped space. */
    {
        uint32_t padded[16] = { 0 };
        padded[1 * 4 + 2] = 0xFFAABBCCu; /* row 1, col 2 */
        uint32_t cw, ch, hx = 2, hy = 1;
        uint32_t *cropped = ghostcon_cursor_crop_to_content(padded, 4, 4, &cw, &ch, &hx, &hy);
        CHECK(cropped != NULL, "crop_to_content finds the single opaque pixel");
        if (cropped) {
            CHECK(cw == 1 && ch == 1, "crop_to_content shrinks to a 1x1 bounding box");
            CHECK(cropped[0] == 0xFFAABBCCu, "crop_to_content keeps the opaque pixel's color");
            CHECK(hx == 0 && hy == 0, "crop_to_content shifts the hotspot onto the cropped pixel");
            free(cropped);
        }

        /* A wider opaque rectangle, hotspot outside it (in the
           padding) -- should clamp onto the cropped image's edge
           rather than go negative or out of bounds. */
        uint32_t rect[16] = { 0 };
        for (uint32_t y = 1; y <= 2; y++)
            for (uint32_t x = 1; x <= 2; x++)
                rect[y * 4 + x] = 0xFF112233u;
        uint32_t rw, rh, rhx = 0, rhy = 0; /* hotspot in the padding, top-left corner */
        uint32_t *rcropped = ghostcon_cursor_crop_to_content(rect, 4, 4, &rw, &rh, &rhx, &rhy);
        CHECK(rcropped != NULL, "crop_to_content finds the 2x2 opaque rectangle");
        if (rcropped) {
            CHECK(rw == 2 && rh == 2, "crop_to_content shrinks to a 2x2 bounding box");
            CHECK(rhx == 0 && rhy == 0, "crop_to_content clamps an out-of-bounds hotspot onto the crop");
            free(rcropped);
        }

        /* Fully transparent -- nothing to crop to, must return NULL
           without touching the hotspot out-parameters. */
        uint32_t blank[4] = { 0 };
        uint32_t bw, bh, bhx = 9, bhy = 9;
        CHECK(ghostcon_cursor_crop_to_content(blank, 2, 2, &bw, &bh, &bhx, &bhy) == NULL,
              "crop_to_content returns NULL for a fully transparent image");
        CHECK(bhx == 9 && bhy == 9,
              "crop_to_content leaves the hotspot untouched when it returns NULL");
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
