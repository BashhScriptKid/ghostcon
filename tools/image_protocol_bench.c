/* image_protocol_bench -- a plain terminal client (no libghostcon
 * dependency at all, same spirit as tools/tui_torture.c) that emits
 * real Kitty graphics protocol (APC _G), iTerm2 inline image protocol
 * (OSC 1337 File=), and Sixel (DCS q) sequences to stdout, choreographed
 * across pages so each page exercises one specific mechanism of one
 * specific image protocol end-to-end through the real
 * parser -> screen -> renderer pipeline.
 *
 * All test images are generated synthetically at runtime (checkerboard,
 * gradient, alpha ramp) -- no external image files or image libraries
 * are used. iTerm2's File= payload must be a real decodable image
 * format, so this tool includes a minimal from-scratch PNG encoder
 * (stored/uncompressed DEFLATE blocks -- no zlib dependency).
 *
 * Usage:
 *   image_protocol_bench --list            list all pages
 *   image_protocol_bench <page>             run just that one page
 *   image_protocol_bench [--all]            run every page in sequence
 *
 * In --all / no-args mode, each page waits for Enter before advancing
 * so you have time to look and call out which one (if any) glitches.
 */

#define _DEFAULT_SOURCE /* usleep() under -std=c11 without this */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define ESC "\x1b"

/* ------------------------------------------------------------------ */
/* Terminal setup / banner (same idiom as tui_torture.c: replies that   */
/* protocols write back into our stdin -- e.g. Kitty's _Gi=...;OK acks  */
/* -- would otherwise get ECHOed back onto the display as literal text) */
/* ------------------------------------------------------------------ */

static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void
restore_termios(void)
{
    if (g_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static void
disable_echo(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0)
        return;
    g_termios_saved = true;
    atexit(restore_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void
banner(const char *title, const char *desc)
{
    printf(ESC "[2J" ESC "[H");
    printf(ESC "[1;97;44m %s " ESC "[0m\r\n", title);
    printf(ESC "[36m%s" ESC "[0m\r\n\r\n", desc);
    fflush(stdout);
    printf(ESC "[2J" ESC "[H");
    fflush(stdout);
}

/* Kitty graphics (and iTerm2/sixel) never move the cursor past a
   displayed image themselves -- that's the client's job, same as any
   real image-aware tool (kitten icat, chafa) does by computing rows
   from the image's pixel height vs. the terminal's actual cell size.
   A page that just prints a fixed "\r\n\r\n" after an image only
   happens to clear it for whatever specific size that page was tuned
   to -- found live: a fixed two-line gap left kitty_basic_rgb's own
   label text overlapping the bottom of its 64px-tall image on an
   18px-cell real terminal. */
static int g_cell_h = 20; /* fallback if the pty doesn't report pixel size */

static void
query_cell_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_ypixel > 0)
        g_cell_h = ws.ws_ypixel / ws.ws_row;
}

/* Prints enough newlines to move the cursor past an image height_px
   pixels tall. */
static void
advance_past_image(int height_px)
{
    int rows = (height_px + g_cell_h - 1) / g_cell_h;
    if (rows < 1)
        rows = 1;
    for (int i = 0; i < rows; i++)
        printf("\r\n");
}

static void
wait_enter(void)
{
    printf("\r\n" ESC "[90m-- done, press Enter to continue --" ESC "[0m");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
}

/* ------------------------------------------------------------------ */
/* base64                                                              */
/* ------------------------------------------------------------------ */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
b64_encode(const uint8_t *data, size_t len, size_t *out_len)
{
    size_t olen = ((len + 2) / 3) * 4;
    char *out = malloc(olen + 1);
    size_t i = 0, o = 0;

    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = b64_table[(v >> 18) & 0x3F];
        out[o++] = b64_table[(v >> 12) & 0x3F];
        out[o++] = b64_table[(v >> 6) & 0x3F];
        out[o++] = b64_table[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = b64_table[(v >> 18) & 0x3F];
        out[o++] = b64_table[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = b64_table[(v >> 18) & 0x3F];
        out[o++] = b64_table[(v >> 12) & 0x3F];
        out[o++] = b64_table[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

/* ------------------------------------------------------------------ */
/* Minimal PNG encoder (stored/uncompressed DEFLATE -- no zlib)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static void
buf_append(buf_t *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void
buf_u32be(buf_t *b, uint32_t v)
{
    uint8_t be[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    buf_append(b, be, 4);
}

static uint32_t
crc32_of(const uint8_t *buf, size_t len)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t
adler32_of(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void
png_chunk(buf_t *out, const char *type, const uint8_t *data, uint32_t len)
{
    buf_u32be(out, len);
    size_t start = out->len;
    buf_append(out, type, 4);
    if (len)
        buf_append(out, data, len);
    uint32_t crc = crc32_of(out->data + start, out->len - start);
    buf_u32be(out, crc);
}

/* pixels is w*h*(rgba?4:3) bytes, row-major, no padding. */
static uint8_t *
build_png(int w, int h, bool rgba, const uint8_t *pixels, size_t *out_len)
{
    int bpp = rgba ? 4 : 3;
    size_t raw_len = (size_t)(bpp * w + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    size_t p = 0;
    for (int y = 0; y < h; y++) {
        raw[p++] = 0; /* filter: None */
        memcpy(raw + p, pixels + (size_t)y * (size_t)w * (size_t)bpp, (size_t)w * (size_t)bpp);
        p += (size_t)w * (size_t)bpp;
    }

    buf_t z = {0};
    uint8_t zh[2] = { 0x78, 0x01 };
    buf_append(&z, zh, 2);

    size_t off = 0;
    do {
        size_t take = raw_len - off;
        if (take > 65535)
            take = 65535;
        bool final = (off + take) == raw_len;
        uint8_t hdr = final ? 1 : 0;
        buf_append(&z, &hdr, 1);
        uint16_t len16 = (uint16_t)take;
        uint16_t nlen16 = (uint16_t)~len16;
        uint8_t lenb[4] = {
            (uint8_t)(len16), (uint8_t)(len16 >> 8),
            (uint8_t)(nlen16), (uint8_t)(nlen16 >> 8),
        };
        buf_append(&z, lenb, 4);
        buf_append(&z, raw + off, take);
        off += take;
    } while (off < raw_len);

    uint32_t ad = adler32_of(raw, raw_len);
    buf_u32be(&z, ad);
    free(raw);

    buf_t png = {0};
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    buf_append(&png, sig, 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;                   /* bit depth */
    ihdr[9] = rgba ? 6 : 2;        /* color type: 6=RGBA, 2=RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(&png, "IHDR", ihdr, 13);
    png_chunk(&png, "IDAT", z.data, (uint32_t)z.len);
    png_chunk(&png, "IEND", NULL, 0);

    free(z.data);
    *out_len = png.len;
    return png.data;
}

/* ------------------------------------------------------------------ */
/* Synthetic pixel patterns                                            */
/* ------------------------------------------------------------------ */

static void
gen_checkerboard(uint8_t *px, int w, int h, int bpp, int cell,
                  uint8_t r1, uint8_t g1, uint8_t b1,
                  uint8_t r2, uint8_t g2, uint8_t b2)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool on = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * (size_t)bpp;
            p[0] = on ? r1 : r2;
            p[1] = on ? g1 : g2;
            p[2] = on ? b1 : b2;
            if (bpp == 4)
                p[3] = 255;
        }
    }
}

static void
gen_gradient(uint8_t *px, int w, int h, int bpp)
{
    int wd = w > 1 ? w - 1 : 1;
    int hd = h > 1 ? h - 1 : 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * (size_t)bpp;
            p[0] = (uint8_t)((x * 255) / wd);
            p[1] = (uint8_t)((y * 255) / hd);
            p[2] = 128;
            if (bpp == 4)
                p[3] = 255;
        }
    }
}

static void
gen_alpha_ramp(uint8_t *px, int w, int h)
{
    int wd = w > 1 ? w - 1 : 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = 255; p[1] = 60; p[2] = 60;
            p[3] = (uint8_t)((x * 255) / wd);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Kitty graphics protocol (APC _G...ST)                               */
/* ------------------------------------------------------------------ */

#define KITTY_CHUNK 4096 /* max base64 bytes per chunk, per spec */

static void
kitty_send(const char *keys, const uint8_t *payload, size_t len)
{
    size_t b64_len = 0;
    char *b64 = b64_encode(payload, len, &b64_len);
    size_t off = 0;
    bool is_first = true;

    do {
        size_t take = b64_len - off;
        if (take > KITTY_CHUNK)
            take = KITTY_CHUNK;
        bool more = (off + take) < b64_len;

        if (is_first)
            printf(ESC "_G%s%s;", keys, more ? ",m=1" : "");
        else
            printf(ESC "_Gm=%d;", more ? 1 : 0);
        fwrite(b64 + off, 1, take, stdout);
        printf(ESC "\\");

        off += take;
        is_first = false;
    } while (off < b64_len);

    fflush(stdout);
    free(b64);
}

/* ------------------------------------------------------------------ */
/* Sixel (DCS q ... ST)                                                */
/* ------------------------------------------------------------------ */

static void
emit_sixel_run(const uint8_t *bits, int width)
{
    int i = 0;
    while (i < width) {
        int j = i + 1;
        while (j < width && bits[j] == bits[i])
            j++;
        int run = j - i;
        char ch = (char)(63 + bits[i]);
        if (run > 3)
            printf("!%d%c", run, ch);
        else
            for (int k = 0; k < run; k++)
                putchar(ch);
        i = j;
    }
}

/* idx is a w*h array of palette indices (row-major). */
static void
sixel_send(int width, int height, const uint8_t *idx,
           const uint8_t palette[][3], int ncolors)
{
    printf(ESC "Pq" "\"1;1;%d;%d", width, height);
    for (int c = 0; c < ncolors; c++) {
        printf("#%d;2;%d;%d;%d", c,
               (palette[c][0] * 100 + 127) / 255,
               (palette[c][1] * 100 + 127) / 255,
               (palette[c][2] * 100 + 127) / 255);
    }

    uint8_t *bits = malloc((size_t)width);
    for (int y0 = 0; y0 < height; y0 += 6) {
        int rows = height - y0;
        if (rows > 6)
            rows = 6;
        for (int c = 0; c < ncolors; c++) {
            bool any = false;
            for (int x = 0; x < width; x++) {
                uint8_t b = 0;
                for (int r = 0; r < rows; r++) {
                    if (idx[(size_t)(y0 + r) * (size_t)width + (size_t)x] == c) {
                        b |= (uint8_t)(1u << r);
                        any = true;
                    }
                }
                bits[x] = b;
            }
            if (!any)
                continue;
            printf("#%d", c);
            emit_sixel_run(bits, width);
            printf("$");
        }
        if (y0 + 6 < height)
            printf("-");
    }
    free(bits);
    printf(ESC "\\");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static void
page_kitty_basic_rgb(void)
{
    banner("kitty_basic_rgb", "Kitty graphics: raw RGB (f=24), immediate transmit+display (a=T).");
    int w = 64, h = 64, bpp = 3;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_checkerboard(px, w, h, bpp, 8, 220, 60, 60, 30, 30, 140);
    char keys[128];
    snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=1,C=1", w, h);
    kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
    free(px);
    advance_past_image(h);
    printf("\r\n  raw RGB checkerboard, 64x64, id=1\r\n");
}

static void
page_kitty_rgba_alpha(void)
{
    banner("kitty_rgba_alpha", "Kitty graphics: RGBA (f=32) with a left-to-right alpha ramp over text.");
    printf("background text to show transparency through -----------------\r\n");
    printf("background text to show transparency through -----------------\r\n");
    int w = 96, h = 48, bpp = 4;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_alpha_ramp(px, w, h);
    char keys[128];
    snprintf(keys, sizeof keys, "a=T,f=32,s=%d,v=%d,i=2,C=1", w, h);
    kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
    free(px);
    advance_past_image(h);
    printf("\r\n  RGBA alpha ramp (opaque->transparent, left to right), 96x48, id=2\r\n");
}

static void
page_kitty_chunked_large(void)
{
    banner("kitty_chunked_large", "Kitty graphics: image large enough to force >4KB base64 chunking (m=1/m=0).");
    int w = 256, h = 256, bpp = 3;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_gradient(px, w, h, bpp);
    char keys[128];
    snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=3,C=1", w, h);
    kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
    free(px);
    advance_past_image(h);
    printf("\r\n  256x256 gradient sent as multiple m=1 chunks, id=3\r\n");
}

static void
page_kitty_placement_crop(void)
{
    banner("kitty_placement_crop", "Kitty graphics: transmit once (a=t), then multiple placements (a=p): full, cropped, cell-scaled.");
    int w = 128, h = 128, bpp = 3;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_checkerboard(px, w, h, bpp, 4, 240, 200, 40, 40, 40, 40);
    char keys[128];
    snprintf(keys, sizeof keys, "a=t,f=24,s=%d,v=%d,i=4,q=2,C=1", w, h);
    kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
    free(px);

    printf(ESC "_Ga=p,i=4,p=1,C=1" ESC "\\");
    advance_past_image(h); /* full size: 128x128 */
    printf(ESC "_Ga=p,i=4,p=2,x=0,y=0,w=64,h=64,C=1" ESC "\\");
    advance_past_image(64); /* cropped to 64x64 */
    printf(ESC "_Ga=p,i=4,p=3,c=10,r=5,C=1" ESC "\\");
    advance_past_image(5 * g_cell_h); /* scaled to exactly 5 cell-rows */
    printf("\r\n  placements of id=4: full (p=1), top-left 64x64 crop (p=2), 10x5-cell scaled (p=3)\r\n");
}

static void
page_kitty_zindex(void)
{
    banner("kitty_zindex", "Kitty graphics: overlapping placements with different z (behind vs in front of text).");
    int w = 40, h = 20, bpp = 3;
    uint8_t *behind = malloc((size_t)w * (size_t)h * (size_t)bpp);
    uint8_t *front = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_checkerboard(behind, w, h, bpp, 2, 40, 40, 200, 10, 10, 80);
    gen_checkerboard(front, w, h, bpp, 2, 200, 40, 40, 80, 10, 10);

    char keys[128];
    snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=5,z=-1,C=1", w, h);
    kitty_send(keys, behind, (size_t)w * (size_t)h * (size_t)bpp);
    printf("\r\n");
    printf("THIS TEXT SHOULD BE ON TOP OF THE BLUE TILE AND BEHIND THE RED ONE\r\n");
    snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=6,z=1,C=1", w, h);
    kitty_send(keys, front, (size_t)w * (size_t)h * (size_t)bpp);
    free(behind);
    free(front);
    advance_past_image(h);
    printf("\r\n  z=-1 image (blue, id=5) behind text, z=1 image (red, id=6) in front\r\n");
}

static void
page_kitty_delete(void)
{
    banner("kitty_delete", "Kitty graphics: transmit+display several images, then delete one by id (a=d,d=i).");
    int w = 32, h = 32, bpp = 3;
    for (int n = 0; n < 3; n++) {
        uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
        gen_checkerboard(px, w, h, bpp, 4, (uint8_t)(60 + n * 60), 40, 200, 20, 20, 40);
        char keys[128];
        snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=%d,C=1", w, h, 10 + n);
        kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
        free(px);
        printf("  ");
    }
    advance_past_image(h);
    printf("\r\n  three images shown above (id=10,11,12) -- deleting id=11 in 2s\r\n");
    fflush(stdout);
    usleep(2000000);
    printf(ESC "_Ga=d,d=i,i=11" ESC "\\");
    printf("\r\n  id=11 deleted -- middle image should now be gone\r\n");
}

static void
page_iterm_inline_png(void)
{
    banner("iterm_inline_png", "iTerm2 inline images: OSC 1337 File= carrying a real PNG (stored-DEFLATE, no zlib dep).");
    int w = 48, h = 48, bpp = 3;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_gradient(px, w, h, bpp);
    size_t png_len = 0;
    uint8_t *png = build_png(w, h, false, px, &png_len);
    free(px);
    size_t b64_len = 0;
    char *b64 = b64_encode(png, png_len, &b64_len);
    free(png);
    printf(ESC "]1337;File=inline=1;width=%dpx;height=%dpx;preserveAspectRatio=1:%s" ESC "\\", w, h, b64);
    free(b64);
    advance_past_image(h);
    printf("\r\n  48x48 gradient PNG, %zu bytes\r\n", png_len);
}

static void
page_iterm_size_variants(void)
{
    banner("iterm_size_variants", "iTerm2 inline images: same PNG requested at different cell/pixel size hints.");
    int w = 32, h = 32, bpp = 4;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_checkerboard(px, w, h, bpp, 4, 250, 250, 60, 30, 120, 30);
    size_t png_len = 0;
    uint8_t *png = build_png(w, h, true, px, &png_len);
    free(px);
    size_t b64_len = 0;
    char *b64 = b64_encode(png, png_len, &b64_len);

    printf(ESC "]1337;File=inline=1;width=8;height=4:%s" ESC "\\", b64);
    advance_past_image(4 * g_cell_h); /* 4-cell-row hint */
    printf(ESC "]1337;File=inline=1;width=auto;height=auto;preserveAspectRatio=0:%s" ESC "\\", b64);
    free(b64);
    free(png);
    advance_past_image(h); /* natural size, approximating the auto-scaled result */
    printf("\r\n  same 32x32 RGBA checkerboard: 8x4-cell hint, then auto/stretched\r\n");
}

static void
page_sixel_basic(void)
{
    banner("sixel_basic", "Sixel (DCS q): small 4-color palette checkerboard.");
    int w = 60, h = 36;
    static const uint8_t pal[4][3] = {
        { 220, 60, 60 }, { 40, 40, 180 }, { 240, 220, 40 }, { 20, 20, 20 },
    };
    uint8_t *idx = malloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            idx[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(((x / 6) + (y / 6)) % 4);
    sixel_send(w, h, idx, pal, 4);
    free(idx);
    advance_past_image(h);
    printf("\r\n  60x36 4-color sixel checkerboard\r\n");
}

static void
page_sixel_bands(void)
{
    banner("sixel_bands", "Sixel: taller image spanning multiple 6-row sixel bands, horizontal color bands.");
    int w = 80, h = 60;
    static const uint8_t pal[6][3] = {
        { 200, 40, 40 }, { 200, 120, 40 }, { 200, 200, 40 },
        { 40, 180, 40 }, { 40, 120, 200 }, { 140, 40, 200 },
    };
    uint8_t *idx = malloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            idx[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)((y * 6) / h);
    sixel_send(w, h, idx, pal, 6);
    free(idx);
    advance_past_image(h);
    printf("\r\n  80x60 sixel, 6 horizontal color bands crossing multiple sixel bands\r\n");
}

static void
page_combined_stress(void)
{
    banner("combined_stress", "All three protocols back-to-back, interleaved with normal scrolling text.");
    for (int i = 0; i < 3; i++)
        printf("log line %d before image\r\n", i);

    int w = 32, h = 32, bpp = 3;
    uint8_t *px = malloc((size_t)w * (size_t)h * (size_t)bpp);
    gen_checkerboard(px, w, h, bpp, 4, 200, 60, 60, 30, 30, 30);
    char keys[128];
    snprintf(keys, sizeof keys, "a=T,f=24,s=%d,v=%d,i=90,C=1", w, h);
    kitty_send(keys, px, (size_t)w * (size_t)h * (size_t)bpp);
    free(px);
    advance_past_image(h);

    uint8_t *px2 = malloc((size_t)w * (size_t)h * 3);
    gen_gradient(px2, w, h, 3);
    size_t png_len = 0;
    uint8_t *png = build_png(w, h, false, px2, &png_len);
    free(px2);
    size_t b64_len = 0;
    char *b64 = b64_encode(png, png_len, &b64_len);
    free(png);
    printf(ESC "]1337;File=inline=1;width=%dpx;height=%dpx:%s" ESC "\\", w, h, b64);
    free(b64);
    advance_past_image(h);

    static const uint8_t pal[2][3] = { { 240, 240, 40 }, { 20, 20, 20 } };
    uint8_t *idx = malloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            idx[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)((x + y) % 2);
    sixel_send(w, h, idx, pal, 2);
    free(idx);
    advance_past_image(h);

    for (int i = 0; i < 5; i++)
        printf("\r\nlog line after, scrolling %d", i);
    printf("\r\n\r\n  kitty + iterm2 + sixel images each shown once above\r\n");
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *desc;
    void (*fn)(void);
} page_t;

static const page_t pages[] = {
    { "kitty_basic_rgb",       "Kitty: raw RGB transmit+display",              page_kitty_basic_rgb },
    { "kitty_rgba_alpha",      "Kitty: RGBA alpha ramp over text",             page_kitty_rgba_alpha },
    { "kitty_chunked_large",   "Kitty: >4KB base64 chunked transmit",          page_kitty_chunked_large },
    { "kitty_placement_crop",  "Kitty: transmit-once + multi placement/crop",  page_kitty_placement_crop },
    { "kitty_zindex",          "Kitty: z-order vs text (behind/in front)",     page_kitty_zindex },
    { "kitty_delete",          "Kitty: multiple images then delete by id",     page_kitty_delete },
    { "iterm_inline_png",      "iTerm2: OSC 1337 File= real PNG payload",      page_iterm_inline_png },
    { "iterm_size_variants",   "iTerm2: cell/pixel/auto size hints",           page_iterm_size_variants },
    { "sixel_basic",           "Sixel: small palette checkerboard",            page_sixel_basic },
    { "sixel_bands",           "Sixel: multi-band tall image",                 page_sixel_bands },
    { "combined_stress",       "Kitty + iTerm2 + Sixel interleaved w/ scroll", page_combined_stress },
};
#define NUM_PAGES (sizeof(pages) / sizeof(pages[0]))

static void
list_pages(void)
{
    for (size_t i = 0; i < NUM_PAGES; i++)
        printf("%-20s %s\n", pages[i].name, pages[i].desc);
}

static void
run_page(const page_t *p, bool interactive)
{
    p->fn();
    if (interactive)
        wait_enter();
    else
        fflush(stdout);
}

int
main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "--list") == 0 || strcmp(argv[1], "-l") == 0)) {
        list_pages();
        return 0;
    }

    disable_echo();
    query_cell_size();

    if (argc > 1 && strcmp(argv[1], "--all") != 0) {
        for (size_t i = 0; i < NUM_PAGES; i++) {
            if (strcmp(argv[1], pages[i].name) == 0) {
                run_page(&pages[i], false);
                printf("\r\n");
                return 0;
            }
        }
        fprintf(stderr, "image_protocol_bench: unknown page '%s' (try --list)\n", argv[1]);
        return 1;
    }

    for (size_t i = 0; i < NUM_PAGES; i++)
        run_page(&pages[i], true);

    printf(ESC "[2J" ESC "[H" "all pages done.\r\n");
    return 0;
}
