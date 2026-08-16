#include "ghostcon/core/cursor_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian primitive reads -- BMP and Xcursor are both LE on     */
/* disk regardless of host byte order.                                 */
/* ------------------------------------------------------------------ */

static uint32_t
rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t
rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int32_t
rd_i32le(const uint8_t *p)
{
    return (int32_t)rd_u32le(p);
}

static void
wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void
wr_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

/* Reads the whole file into a malloc'd buffer. NULL on failure.
   *out_len is the file size. Shared by both decoders below. */
static uint8_t *
read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len > 0 ? (size_t)len : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* BMP                                                                  */
/* ------------------------------------------------------------------ */

#define BMP_FILE_HEADER_SIZE 14
#define BMP_DIB_HEADER_SIZE 40 /* BITMAPINFOHEADER -- the only variant this decoder reads/writes */

uint32_t *
ghostcon_cursor_load_bmp(const char *path, uint32_t *out_w, uint32_t *out_h)
{
    size_t len;
    uint8_t *buf = read_whole_file(path, &len);
    if (!buf)
        return NULL;

    if (len < BMP_FILE_HEADER_SIZE + BMP_DIB_HEADER_SIZE || buf[0] != 'B' || buf[1] != 'M') {
        free(buf);
        return NULL;
    }

    uint32_t pixel_offset = rd_u32le(buf + 10);
    uint32_t dib_size = rd_u32le(buf + 14);
    if (dib_size < BMP_DIB_HEADER_SIZE) {
        free(buf); /* OS/2 or other pre-Windows-3.0 header variant -- not supported */
        return NULL;
    }

    int32_t width = rd_i32le(buf + 18);
    int32_t height_field = rd_i32le(buf + 22);
    uint16_t planes = rd_u16le(buf + 26);
    uint16_t bpp = rd_u16le(buf + 28);
    uint32_t compression = rd_u32le(buf + 30);

    bool top_down = height_field < 0;
    int32_t height = top_down ? -height_field : height_field;

    if (planes != 1 || compression != 0 /* BI_RGB */ || (bpp != 24 && bpp != 32) ||
        width <= 0 || height <= 0) {
        free(buf);
        return NULL;
    }

    uint32_t row_size = (((uint32_t)bpp * (uint32_t)width + 31) / 32) * 4;
    size_t need = (size_t)pixel_offset + (size_t)row_size * (size_t)height;
    if (need > len) {
        free(buf);
        return NULL;
    }

    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    uint32_t *pixels = malloc((size_t)w * h * sizeof(uint32_t));
    if (!pixels) {
        free(buf);
        return NULL;
    }

    for (uint32_t y = 0; y < h; y++) {
        /* BMP's default (positive height field) storage is bottom-up;
           normalize to top-down output regardless of source order. */
        uint32_t src_row = top_down ? y : (h - 1 - y);
        const uint8_t *row = buf + pixel_offset + (size_t)src_row * row_size;
        uint32_t *out_row = pixels + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px = row + (size_t)x * (bpp / 8);
            uint8_t b = px[0], g = px[1], r = px[2];
            uint8_t a = (bpp == 32) ? px[3] : 0xFF; /* 24-bit BMP has no alpha -- fully opaque */
            out_row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    free(buf);
    *out_w = w;
    *out_h = h;
    return pixels;
}

bool
ghostcon_cursor_write_bmp(const char *path, const uint32_t *pixels, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return false;

    uint32_t row_size = w * 4; /* 32bpp is always 4-byte aligned per row already */
    uint32_t pixel_offset = BMP_FILE_HEADER_SIZE + BMP_DIB_HEADER_SIZE;
    uint32_t file_size = pixel_offset + row_size * h;

    uint8_t *buf = malloc(file_size);
    if (!buf)
        return false;

    memset(buf, 0, BMP_FILE_HEADER_SIZE + BMP_DIB_HEADER_SIZE);
    buf[0] = 'B';
    buf[1] = 'M';
    wr_u32le(buf + 2, file_size);
    wr_u32le(buf + 10, pixel_offset);

    wr_u32le(buf + 14, BMP_DIB_HEADER_SIZE);
    wr_u32le(buf + 18, w);
    /* Negative height = top-down storage -- avoids needing to flip rows
       on write (we already hold pixels top-down internally) or on a
       later read-back via ghostcon_cursor_load_bmp(), which handles
       both signs. Well-supported (Windows since 3.0, every modern
       reader including this one) despite being the less common case. */
    wr_u32le(buf + 22, (uint32_t)(-(int32_t)h));
    wr_u16le(buf + 26, 1);  /* planes */
    wr_u16le(buf + 28, 32); /* bpp */
    wr_u32le(buf + 30, 0);  /* BI_RGB, uncompressed */
    wr_u32le(buf + 34, row_size * h);

    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf + pixel_offset + (size_t)y * row_size;
        const uint32_t *in_row = pixels + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = in_row[x];
            row[x * 4 + 0] = (uint8_t)(px & 0xff);         /* B */
            row[x * 4 + 1] = (uint8_t)((px >> 8) & 0xff);  /* G */
            row[x * 4 + 2] = (uint8_t)((px >> 16) & 0xff); /* R */
            row[x * 4 + 3] = (uint8_t)((px >> 24) & 0xff); /* A */
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return false;
    }
    size_t n = fwrite(buf, 1, file_size, f);
    fclose(f);
    free(buf);
    return n == file_size;
}

/* ------------------------------------------------------------------ */
/* Xcursor                                                              */
/*                                                                       */
/* Format (see PLAN.md's "Cursor sprite" section for the reference this */
/* mirrors): magic "Xcur" + version (u32) + ntoc (u32), then `ntoc`     */
/* table-of-contents entries (type u32, subtype u32, position u32),     */
/* each pointing to a chunk. An image chunk (type 0xfffd0002) has its   */
/* own header (header_size, type, subtype, version -- all u32) followed */
/* by width/height/xhot/yhot/delay (all u32), then width*height raw     */
/* ARGB32 pixels.                                                       */
/* ------------------------------------------------------------------ */

/* "Xcur" (bytes 0x58,0x63,0x75,0x72 on disk) read as a little-endian
   u32 -- verified against a real system Xcursor file's actual header
   bytes, not just derived by hand: an earlier version of this constant
   (0x72754358) was a transposition typo that happened to match this
   file's own hand-built synthetic test (which used the same wrong
   value for both writing and reading), passing that test while
   rejecting every real Xcursor file on the system as invalid -- found
   live via xcursor2bmp against a real theme. */
#define XCURSOR_MAGIC 0x72756358u
#define XCURSOR_IMAGE_TYPE 0xfffd0002u
#define XCURSOR_FILE_HEADER_SIZE 16 /* magic, header_size, version, ntoc -- all u32 */
#define XCURSOR_TOC_ENTRY_SIZE 12  /* type, subtype, position -- all u32 */
#define XCURSOR_IMAGE_HEADER_SIZE 36 /* chunk header (16) + width/height/xhot/yhot/delay (20) */

static uint32_t *
decode_xcursor_buf(const uint8_t *buf, size_t len,
                    uint32_t *out_w, uint32_t *out_h, uint32_t *out_hot_x, uint32_t *out_hot_y)
{
    if (len < XCURSOR_FILE_HEADER_SIZE || rd_u32le(buf) != XCURSOR_MAGIC)
        return NULL;

    uint32_t header_size = rd_u32le(buf + 4);
    uint32_t ntoc = rd_u32le(buf + 12);
    if (header_size > len)
        return NULL;

    /* Find the image-type TOC entry with the largest subtype (pixel
       size) -- see this file's own header doc comment on why "largest
       wins" rather than picking a specific DPI match. */
    uint32_t best_subtype = 0;
    uint32_t best_position = 0;
    bool found = false;
    size_t toc_off = header_size;
    for (uint32_t i = 0; i < ntoc; i++) {
        size_t entry_off = toc_off + (size_t)i * XCURSOR_TOC_ENTRY_SIZE;
        if (entry_off + XCURSOR_TOC_ENTRY_SIZE > len)
            break;
        uint32_t type = rd_u32le(buf + entry_off);
        uint32_t subtype = rd_u32le(buf + entry_off + 4);
        uint32_t position = rd_u32le(buf + entry_off + 8);
        if (type == XCURSOR_IMAGE_TYPE && (!found || subtype > best_subtype)) {
            best_subtype = subtype;
            best_position = position;
            found = true;
        }
    }
    if (!found)
        return NULL;

    if ((size_t)best_position + XCURSOR_IMAGE_HEADER_SIZE > len)
        return NULL;
    const uint8_t *chunk = buf + best_position;
    uint32_t width = rd_u32le(chunk + 16);
    uint32_t height = rd_u32le(chunk + 20);
    uint32_t xhot = rd_u32le(chunk + 24);
    uint32_t yhot = rd_u32le(chunk + 28);

    if (width == 0 || height == 0 || width > 4096 || height > 4096)
        return NULL; /* sanity bound -- not a real cursor size, likely a corrupt/truncated file */

    size_t pixels_off = (size_t)best_position + XCURSOR_IMAGE_HEADER_SIZE;
    size_t pixels_need = (size_t)width * height * 4;
    if (pixels_off + pixels_need > len)
        return NULL;

    uint32_t *pixels = malloc((size_t)width * height * sizeof(uint32_t));
    if (!pixels)
        return NULL;

    /* Xcursor image pixels are already stored as raw ARGB32
       (premultiplied), byte order matching this project's own ARGB8888
       convention (see core/kms.c's cursor buffer format) -- a direct
       word-for-word copy, no channel reordering needed. */
    memcpy(pixels, buf + pixels_off, pixels_need);

    *out_w = width;
    *out_h = height;
    *out_hot_x = xhot;
    *out_hot_y = yhot;
    return pixels;
}

uint32_t *
ghostcon_cursor_load_xcursor(const char *theme_dir, const char *name,
                              uint32_t *out_w, uint32_t *out_h,
                              uint32_t *out_hot_x, uint32_t *out_hot_y)
{
    char path[1024];

    /* Try `theme_dir` as the cursors/ directory itself first (cheaper
       than stat()-ing to decide), then fall back to `theme_dir/cursors/`
       -- see this function's own doc comment in cursor_image.h. */
    snprintf(path, sizeof(path), "%s/%s", theme_dir, name);
    size_t len;
    uint8_t *buf = read_whole_file(path, &len);
    if (!buf) {
        snprintf(path, sizeof(path), "%s/cursors/%s", theme_dir, name);
        buf = read_whole_file(path, &len);
        if (!buf)
            return NULL;
    }

    uint32_t *pixels = decode_xcursor_buf(buf, len, out_w, out_h, out_hot_x, out_hot_y);
    free(buf);
    return pixels;
}

uint32_t *
ghostcon_cursor_crop_to_content(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                                 uint32_t *out_w, uint32_t *out_h,
                                 uint32_t *hot_x, uint32_t *hot_y)
{
    if (!src || src_w == 0 || src_h == 0)
        return NULL;

    uint32_t min_x = src_w, max_x = 0, min_y = src_h, max_y = 0;
    bool any = false;
    for (uint32_t y = 0; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            if ((src[y * src_w + x] >> 24) == 0) /* fully transparent */
                continue;
            any = true;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (!any)
        return NULL; /* fully transparent -- nothing to crop to */

    uint32_t crop_w = max_x - min_x + 1;
    uint32_t crop_h = max_y - min_y + 1;
    uint32_t *dst = malloc((size_t)crop_w * crop_h * sizeof(uint32_t));
    if (!dst)
        return NULL;

    for (uint32_t y = 0; y < crop_h; y++)
        memcpy(dst + (size_t)y * crop_w, src + (size_t)(y + min_y) * src_w + min_x,
               (size_t)crop_w * sizeof(uint32_t));

    *out_w = crop_w;
    *out_h = crop_h;
    /* Shift the hotspot to match the new, smaller coordinate space --
       clamped so a hotspot that fell inside the trimmed padding (e.g.
       BMP callers always pass 0,0, which is almost always padding on a
       real asset) lands on the nearest edge of the cropped image
       rather than going negative/out of bounds. */
    *hot_x = *hot_x > min_x ? *hot_x - min_x : 0;
    *hot_y = *hot_y > min_y ? *hot_y - min_y : 0;
    if (*hot_x >= crop_w) *hot_x = crop_w - 1;
    if (*hot_y >= crop_h) *hot_y = crop_h - 1;
    return dst;
}

uint32_t *
ghostcon_cursor_scale(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                       uint32_t dst_w, uint32_t dst_h)
{
    if (!src || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return NULL;

    uint32_t *dst = malloc((size_t)dst_w * dst_h * sizeof(uint32_t));
    if (!dst)
        return NULL;

    /* Fixed-point (16.16) src-per-dst step -- avoids repeated float
       division inside the pixel loop, and dst_x/dst_y * step can't
       overflow a 32-bit accumulator at any cursor-sized dimension. */
    uint32_t step_x = (src_w << 16) / dst_w;
    uint32_t step_y = (src_h << 16) / dst_h;

    for (uint32_t dy = 0; dy < dst_h; dy++) {
        uint32_t sy = (dy * step_y) >> 16;
        if (sy >= src_h)
            sy = src_h - 1;
        for (uint32_t dx = 0; dx < dst_w; dx++) {
            uint32_t sx = (dx * step_x) >> 16;
            if (sx >= src_w)
                sx = src_w - 1;
            dst[dy * dst_w + dx] = src[sy * src_w + sx];
        }
    }
    return dst;
}
