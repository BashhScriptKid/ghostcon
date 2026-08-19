#define _DEFAULT_SOURCE /* strtok_r() under -std=c11 without this */

#include "ghostcon/term/kitty_graphics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* base64 decode -- bounded, rejects invalid input rather than         */
/* skipping it (silently accepting garbage as "decoded fine" would    */
/* make corrupt/adversarial payloads look like valid images).          */
/* ------------------------------------------------------------------ */

static int
b64_val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes in place into *out (caller-allocated, at least
   b64_decoded_max(len) bytes). Returns decoded length, or SIZE_MAX on
   any invalid character / malformed padding. */
static size_t
b64_decode(const char *in, size_t len, uint8_t *out, size_t out_cap)
{
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            if (i >= len)
                return SIZE_MAX; /* truncated group */
            uint8_t c = (uint8_t)in[i++];
            if (c == '=') {
                v[k] = 0;
                pad++;
            } else {
                if (pad)
                    return SIZE_MAX; /* '=' followed by data */
                int d = b64_val(c);
                if (d < 0)
                    return SIZE_MAX;
                v[k] = d;
            }
        }
        if (pad > 2)
            return SIZE_MAX;
        uint32_t triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                           ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        int nbytes = 3 - pad;
        for (int k = 0; k < nbytes; k++) {
            if (o >= out_cap)
                return SIZE_MAX;
            out[o++] = (uint8_t)(triple >> (16 - 8 * k));
        }
        if (pad)
            break; /* padding only valid on the final group */
    }
    return o;
}

static size_t
b64_decoded_max(size_t len)
{
    return ((len + 3) / 4) * 3;
}

/* ------------------------------------------------------------------ */
/* Control-data (key=value,key=value,...) parsing                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char   action;    /* 'a' key value, 0 if absent (defaults to 't') */
    char   medium;    /* 't' key value, 0 if absent (defaults to 'd') */
    int32_t format;   /* 'f', -1 if absent (defaults to 32) */
    int32_t width;    /* 's', -1 if absent */
    int32_t height;   /* 'v', -1 if absent */
    int64_t image_id; /* 'i', -1 if absent */
    int64_t placement_id; /* 'p', -1 if absent */
    int32_t more;     /* 'm', -1 if absent (defaults to 0) */
    int32_t quiet;    /* 'q', -1 if absent (defaults to 0) */
    int32_t z;        /* 'z', -1 if absent (defaults to 0) */
    int32_t crop_x, crop_y, crop_w, crop_h; /* x,y,w,h -- -1 if absent */
    int32_t cell_cols, cell_rows;           /* c,r -- -1 if absent */
    int32_t no_cursor_move; /* 'C', -1 if absent (defaults to 0 -- i.e. DO move) */
    char   delete_mode; /* 'd' key value, 0 if absent */
    bool   valid;
} kitty_control_t;

/* Parses a signed decimal integer, rejecting anything that isn't
   exactly digits (with an optional leading '-'). Returns false on any
   malformed value rather than silently truncating/clamping. */
static bool
parse_i64(const char *s, size_t len, int64_t *out)
{
    if (len == 0)
        return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (len == 1)
            return false;
    }
    int64_t v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        if (v > (INT64_MAX - 9) / 10)
            return false; /* overflow guard */
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

/* control is the substring before the payload's ';' (or the whole
   body if there is no payload section). Mutates a scratch copy, so
   the caller must pass a writable, NUL-terminated buffer. */
static kitty_control_t
kitty_parse_control(char *control)
{
    kitty_control_t c = {0};
    c.format = -1; c.width = -1; c.height = -1;
    c.image_id = -1; c.placement_id = -1; c.more = -1; c.quiet = -1; c.z = -1;
    c.crop_x = -1; c.crop_y = -1; c.crop_w = -1; c.crop_h = -1;
    c.cell_cols = -1; c.cell_rows = -1;
    c.no_cursor_move = -1;
    c.valid = true;

    char *save = NULL;
    for (char *tok = strtok_r(control, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            c.valid = false;
            continue;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        size_t val_len = strlen(val);
        int64_t iv;

        if (strcmp(key, "a") == 0 && val_len == 1) {
            c.action = val[0];
        } else if (strcmp(key, "t") == 0 && val_len == 1) {
            c.medium = val[0];
        } else if (strcmp(key, "d") == 0 && val_len >= 1) {
            c.delete_mode = val[0];
        } else if (strcmp(key, "f") == 0 && parse_i64(val, val_len, &iv)) {
            c.format = (int32_t)iv;
        } else if (strcmp(key, "s") == 0 && parse_i64(val, val_len, &iv)) {
            c.width = (int32_t)iv;
        } else if (strcmp(key, "v") == 0 && parse_i64(val, val_len, &iv)) {
            c.height = (int32_t)iv;
        } else if (strcmp(key, "i") == 0 && parse_i64(val, val_len, &iv)) {
            c.image_id = iv;
        } else if (strcmp(key, "p") == 0 && parse_i64(val, val_len, &iv)) {
            c.placement_id = iv;
        } else if (strcmp(key, "m") == 0 && parse_i64(val, val_len, &iv)) {
            c.more = (int32_t)iv;
        } else if (strcmp(key, "q") == 0 && parse_i64(val, val_len, &iv)) {
            c.quiet = (int32_t)iv;
        } else if (strcmp(key, "z") == 0 && parse_i64(val, val_len, &iv)) {
            c.z = (int32_t)iv;
        } else if (strcmp(key, "x") == 0 && parse_i64(val, val_len, &iv)) {
            c.crop_x = (int32_t)iv;
        } else if (strcmp(key, "y") == 0 && parse_i64(val, val_len, &iv)) {
            c.crop_y = (int32_t)iv;
        } else if (strcmp(key, "w") == 0 && parse_i64(val, val_len, &iv)) {
            c.crop_w = (int32_t)iv;
        } else if (strcmp(key, "h") == 0 && parse_i64(val, val_len, &iv)) {
            c.crop_h = (int32_t)iv;
        } else if (strcmp(key, "c") == 0 && parse_i64(val, val_len, &iv)) {
            c.cell_cols = (int32_t)iv;
        } else if (strcmp(key, "r") == 0 && parse_i64(val, val_len, &iv)) {
            c.cell_rows = (int32_t)iv;
        } else if (strcmp(key, "C") == 0 && parse_i64(val, val_len, &iv)) {
            c.no_cursor_move = (int32_t)iv;
        }
        /* Unrecognized keys (I=, o=, etc.) are accepted and ignored --
           forward-compatible with clients that send extra hints this
           v1 doesn't act on. */
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Acks                                                                 */
/* ------------------------------------------------------------------ */

static void
kitty_ack(ghostcon_kitty_output_fn output_fn, void *userdata,
         int32_t quiet, uint32_t image_id, int64_t placement_id,
         const char *status /* "OK" or "error=CODE:message" */)
{
    if (!output_fn)
        return;
    /* q=1 suppresses OK acks (errors still sent); q=2 suppresses all. */
    bool is_error = strncmp(status, "error", 5) == 0;
    if (quiet >= 2)
        return;
    if (quiet >= 1 && !is_error)
        return;

    char buf[192];
    int n;
    if (placement_id > 0) {
        n = snprintf(buf, sizeof buf, "\x1b_Gi=%u,p=%lld;%s\x1b\\",
                     image_id, (long long)placement_id, status);
    } else {
        n = snprintf(buf, sizeof buf, "\x1b_Gi=%u;%s\x1b\\", image_id, status);
    }
    if (n > 0)
        output_fn(userdata, (const uint8_t *)buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Image store                                                         */
/* ------------------------------------------------------------------ */

static ghostcon_kitty_image_t *
find_image(ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    for (int i = 0; i < GHOSTCON_KITTY_MAX_IMAGES; i++)
        if (kg->images[i].in_use && kg->images[i].id == id)
            return &kg->images[i];
    return NULL;
}

static void
free_image(ghostcon_kitty_graphics_t *kg, ghostcon_kitty_image_t *img)
{
    if (!img->in_use)
        return;
    if (img->pixels) {
        kg->total_bytes -= img->pixel_len;
        free(img->pixels);
    }
    free(img->recv_buf);
    memset(img, 0, sizeof *img);
}

static ghostcon_kitty_image_t *
alloc_image_slot(ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    ghostcon_kitty_image_t *existing = find_image(kg, id);
    if (existing) {
        free_image(kg, existing);
        existing->in_use = true;
        existing->id = id;
        return existing;
    }
    for (int i = 0; i < GHOSTCON_KITTY_MAX_IMAGES; i++) {
        if (!kg->images[i].in_use) {
            kg->images[i].in_use = true;
            kg->images[i].id = id;
            return &kg->images[i];
        }
    }
    return NULL; /* ENOSPC */
}

static void
delete_placements_for_image(ghostcon_kitty_graphics_t *kg, uint32_t image_id,
                            int64_t placement_id /* -1 = all placements of this image */)
{
    for (int i = 0; i < GHOSTCON_KITTY_MAX_PLACEMENTS; i++) {
        ghostcon_kitty_placement_t *p = &kg->placements[i];
        if (!p->in_use || p->image_id != image_id)
            continue;
        if (placement_id >= 0 && p->placement_id != (uint32_t)placement_id)
            continue;
        p->in_use = false;
    }
}

static ghostcon_kitty_placement_t *
alloc_placement_slot(ghostcon_kitty_graphics_t *kg, uint32_t image_id, uint32_t placement_id)
{
    for (int i = 0; i < GHOSTCON_KITTY_MAX_PLACEMENTS; i++) {
        if (kg->placements[i].in_use &&
            kg->placements[i].image_id == image_id &&
            kg->placements[i].placement_id == placement_id) {
            return &kg->placements[i]; /* replace existing placement */
        }
    }
    for (int i = 0; i < GHOSTCON_KITTY_MAX_PLACEMENTS; i++) {
        if (!kg->placements[i].in_use) {
            memset(&kg->placements[i], 0, sizeof kg->placements[i]);
            kg->placements[i].in_use = true;
            return &kg->placements[i];
        }
    }
    return NULL; /* ENOSPC */
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

void
ghostcon_kitty_graphics_init(ghostcon_kitty_graphics_t *kg)
{
    memset(kg, 0, sizeof *kg);
    kg->active_transfer_id = -1;
}

void
ghostcon_kitty_graphics_deinit(ghostcon_kitty_graphics_t *kg)
{
    for (int i = 0; i < GHOSTCON_KITTY_MAX_IMAGES; i++) {
        free(kg->images[i].pixels);
        free(kg->images[i].recv_buf);
    }
    memset(kg, 0, sizeof *kg);
}

/* ------------------------------------------------------------------ */
/* Command handling                                                     */
/* ------------------------------------------------------------------ */

/* Matches Ghostty's kitty/graphics_exec.zig cursor_movement==.after
   (the default -- only C=1 suppresses it): advance past the
   placement's cell footprint, computed from its actual drawn pixel
   size (draw_w_px/draw_h_px, already resolved by the caller from
   crop/c=/r= the same way render/machine.c's render_one_placement
   resolves them for drawing) divided by the renderer's current cell
   size. */
static void
compute_cursor_move(int32_t no_cursor_move, int32_t draw_w_px, int32_t draw_h_px,
                    int32_t anchor_col, int32_t cell_w, int32_t cell_h,
                    ghostcon_kitty_cursor_move_t *out_move)
{
    if (!out_move)
        return;
    if (no_cursor_move == 1 || cell_w <= 0 || cell_h <= 0 ||
        draw_w_px <= 0 || draw_h_px <= 0)
        return;
    int32_t cols = (draw_w_px + cell_w - 1) / cell_w;
    int32_t rows = (draw_h_px + cell_h - 1) / cell_h;
    out_move->moved = true;
    out_move->rows = rows;
    out_move->col = anchor_col + cols + 1;
}

static void
handle_transmit(ghostcon_kitty_graphics_t *kg, const kitty_control_t *ctl,
                const char *payload, size_t payload_len,
                bool also_display, int32_t cursor_col, int32_t cursor_row,
                int32_t cell_w, int32_t cell_h,
                ghostcon_kitty_output_fn output_fn, void *userdata,
                ghostcon_kitty_cursor_move_t *out_move)
{
    int32_t quiet = ctl->quiet >= 0 ? ctl->quiet : 0;

    /* Continuation chunks (m=1 sent previously, this chunk lacking its
       own i=/a=) resolve back to whichever transfer is currently in
       progress -- see the struct's own doc comment on why this can't
       just require i= on every chunk. */
    int64_t resolved_id = ctl->image_id;
    bool resolved_display = also_display;
    if (resolved_id < 0) {
        if (kg->active_transfer_id < 0) {
            kitty_ack(output_fn, userdata, quiet, 0, -1, "error=EINVAL:missing or invalid i=");
            return;
        }
        resolved_id = kg->active_transfer_id;
        resolved_display = kg->active_transfer_display;
    }
    if (resolved_id > UINT32_MAX) {
        kitty_ack(output_fn, userdata, quiet, 0, -1, "error=EINVAL:missing or invalid i=");
        return;
    }
    uint32_t id = (uint32_t)resolved_id;
    also_display = resolved_display;

    if (ctl->medium && ctl->medium != 'd') {
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=EBADF:only direct (t=d) transmission is supported");
        return;
    }

    ghostcon_kitty_image_t *img = find_image(kg, id);
    bool continuing = img && img->receiving;

    /* f= (like s=/v=/i= below) is only meaningful on the first chunk of
       a multi-chunk transfer -- continuation chunks legally omit it,
       so re-deriving bpp from ctl->format on every chunk would silently
       default to 32 (RGBA) instead of the format actually agreed on in
       chunk 1, corrupting the declared size for every later chunk. */
    int32_t bpp;
    if (continuing) {
        bpp = img->bpp;
    } else {
        int32_t fmt = ctl->format >= 0 ? ctl->format : 32;
        if (fmt == 100) {
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EBADF:PNG (f=100) not supported");
            return;
        }
        bpp = (fmt == 24) ? 3 : (fmt == 32) ? 4 : -1;
        if (bpp < 0) {
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EBADF:unsupported format");
            return;
        }
    }

    int32_t width, height;
    if (continuing) {
        width = img->width;
        height = img->height;
    } else {
        width = ctl->width;
        height = ctl->height;
        if (width <= 0 || height <= 0 ||
            (uint32_t)width > GHOSTCON_KITTY_MAX_DIM ||
            (uint32_t)height > GHOSTCON_KITTY_MAX_DIM) {
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EINVAL:missing or out-of-range s=/v=");
            return;
        }
    }
    size_t expected_len = (size_t)width * (size_t)height * (size_t)bpp;
    if (expected_len > GHOSTCON_KITTY_MAX_IMAGE_BYTES) {
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=EINVAL:image too large");
        return;
    }

    size_t decoded_cap = b64_decoded_max(payload_len);
    uint8_t *decoded = decoded_cap ? malloc(decoded_cap) : NULL;
    if (decoded_cap && !decoded) {
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=ENOMEM:allocation failed");
        return;
    }
    size_t decoded_len = payload_len ? b64_decode(payload, payload_len, decoded, decoded_cap) : 0;
    if (decoded_len == SIZE_MAX) {
        free(decoded);
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=EINVAL:malformed base64 payload");
        return;
    }

    bool more = ctl->more == 1;

    if (!continuing) {
        img = alloc_image_slot(kg, id);
        if (!img) {
            free(decoded);
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=ENOSPC:too many images");
            return;
        }
        img->width = width;
        img->height = height;
        img->bpp = bpp;
        if (more) {
            img->recv_buf = malloc(expected_len ? expected_len : 1);
            if (!img->recv_buf) {
                free(decoded);
                free_image(kg, img);
                kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                          "error=ENOMEM:allocation failed");
                return;
            }
            img->recv_len = 0;
            img->receiving = true;
            kg->active_transfer_id = (int64_t)id;
            kg->active_transfer_display = also_display;
        }
    }

    if (more) {
        if (img->recv_len + decoded_len > expected_len) {
            free(decoded);
            free_image(kg, img);
            kg->active_transfer_id = -1;
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EINVAL:payload exceeds declared size");
            return;
        }
        memcpy(img->recv_buf + img->recv_len, decoded, decoded_len);
        img->recv_len += decoded_len;
        free(decoded);
        return; /* no ack for intermediate chunks */
    }

    /* Final (or only) chunk. */
    uint8_t *final_pixels;
    size_t final_len;
    if (img->receiving) {
        if (img->recv_len + decoded_len != expected_len) {
            free(decoded);
            free_image(kg, img);
            kg->active_transfer_id = -1;
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EINVAL:size mismatch");
            return;
        }
        memcpy(img->recv_buf + img->recv_len, decoded, decoded_len);
        free(decoded);
        final_pixels = img->recv_buf;
        final_len = expected_len;
        img->recv_buf = NULL;
        img->receiving = false;
        kg->active_transfer_id = -1;
    } else {
        if (decoded_len != expected_len) {
            free(decoded);
            free_image(kg, img);
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EINVAL:size mismatch");
            return;
        }
        final_pixels = decoded;
        final_len = decoded_len;
    }

    if (kg->total_bytes + final_len > GHOSTCON_KITTY_MAX_TOTAL_BYTES) {
        free(final_pixels);
        free_image(kg, img);
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=ENOSPC:total image memory limit exceeded");
        return;
    }

    img->pixels = final_pixels;
    img->pixel_len = final_len;
    img->generation++;
    kg->total_bytes += final_len;

    if (also_display) {
        ghostcon_kitty_placement_t *p = alloc_placement_slot(
            kg, id, ctl->placement_id > 0 ? (uint32_t)ctl->placement_id : 0);
        if (!p) {
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=ENOSPC:too many placements");
            return;
        }
        p->image_id = id;
        p->placement_id = ctl->placement_id > 0 ? (uint32_t)ctl->placement_id : 0;
        p->anchor_col = cursor_col;
        p->anchor_row = cursor_row;
        p->z = ctl->z >= 0 ? ctl->z : 0;
        p->crop_x = ctl->crop_x >= 0 ? ctl->crop_x : 0;
        p->crop_y = ctl->crop_y >= 0 ? ctl->crop_y : 0;
        p->crop_w = ctl->crop_w >= 0 ? ctl->crop_w : 0;
        p->crop_h = ctl->crop_h >= 0 ? ctl->crop_h : 0;
        p->cell_cols = ctl->cell_cols >= 0 ? ctl->cell_cols : 0;
        p->cell_rows = ctl->cell_rows >= 0 ? ctl->cell_rows : 0;

        int32_t draw_w_px = p->crop_w > 0 ? p->crop_w : img->width;
        int32_t draw_h_px = p->crop_h > 0 ? p->crop_h : img->height;
        if (p->cell_cols > 0 && p->cell_rows > 0) {
            draw_w_px = p->cell_cols * cell_w;
            draw_h_px = p->cell_rows * cell_h;
        }
        compute_cursor_move(ctl->no_cursor_move, draw_w_px, draw_h_px,
                            p->anchor_col, cell_w, cell_h, out_move);
    }

    kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id, "OK");
}

static void
handle_placement(ghostcon_kitty_graphics_t *kg, const kitty_control_t *ctl,
                 int32_t cursor_col, int32_t cursor_row,
                 int32_t cell_w, int32_t cell_h,
                 ghostcon_kitty_output_fn output_fn, void *userdata,
                 ghostcon_kitty_cursor_move_t *out_move)
{
    int32_t quiet = ctl->quiet >= 0 ? ctl->quiet : 0;

    if (ctl->image_id < 0 || ctl->image_id > UINT32_MAX) {
        kitty_ack(output_fn, userdata, quiet, 0, -1, "error=EINVAL:missing or invalid i=");
        return;
    }
    uint32_t id = (uint32_t)ctl->image_id;
    ghostcon_kitty_image_t *img = find_image(kg, id);
    if (!img || !img->pixels) {
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=ENOENT:no image with that id");
        return;
    }

    int32_t cx = ctl->crop_x >= 0 ? ctl->crop_x : 0;
    int32_t cy = ctl->crop_y >= 0 ? ctl->crop_y : 0;
    int32_t cw = ctl->crop_w >= 0 ? ctl->crop_w : 0;
    int32_t ch = ctl->crop_h >= 0 ? ctl->crop_h : 0;
    if (cw > 0 && ch > 0) {
        if (cx < 0 || cy < 0 || cx + cw > img->width || cy + ch > img->height) {
            kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                      "error=EINVAL:crop rect out of bounds");
            return;
        }
    }

    ghostcon_kitty_placement_t *p = alloc_placement_slot(
        kg, id, ctl->placement_id > 0 ? (uint32_t)ctl->placement_id : 0);
    if (!p) {
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id,
                  "error=ENOSPC:too many placements");
        return;
    }
    p->image_id = id;
    p->placement_id = ctl->placement_id > 0 ? (uint32_t)ctl->placement_id : 0;
    p->anchor_col = cursor_col;
    p->anchor_row = cursor_row;
    p->z = ctl->z >= 0 ? ctl->z : 0;
    p->crop_x = cx; p->crop_y = cy; p->crop_w = cw; p->crop_h = ch;
    p->cell_cols = ctl->cell_cols >= 0 ? ctl->cell_cols : 0;
    p->cell_rows = ctl->cell_rows >= 0 ? ctl->cell_rows : 0;

    int32_t draw_w_px = cw > 0 ? cw : img->width;
    int32_t draw_h_px = ch > 0 ? ch : img->height;
    if (p->cell_cols > 0 && p->cell_rows > 0) {
        draw_w_px = p->cell_cols * cell_w;
        draw_h_px = p->cell_rows * cell_h;
    }
    compute_cursor_move(ctl->no_cursor_move, draw_w_px, draw_h_px,
                        p->anchor_col, cell_w, cell_h, out_move);

    kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id, "OK");
}

static void
handle_delete(ghostcon_kitty_graphics_t *kg, const kitty_control_t *ctl,
             ghostcon_kitty_output_fn output_fn, void *userdata)
{
    int32_t quiet = ctl->quiet >= 0 ? ctl->quiet : 0;

    if (ctl->delete_mode == 'a') {
        for (int i = 0; i < GHOSTCON_KITTY_MAX_IMAGES; i++)
            free_image(kg, &kg->images[i]);
        memset(kg->placements, 0, sizeof kg->placements);
        kitty_ack(output_fn, userdata, quiet, 0, -1, "OK");
        return;
    }
    if (ctl->delete_mode == 'i') {
        if (ctl->image_id < 0 || ctl->image_id > UINT32_MAX) {
            kitty_ack(output_fn, userdata, quiet, 0, -1, "error=EINVAL:missing i=");
            return;
        }
        uint32_t id = (uint32_t)ctl->image_id;
        delete_placements_for_image(kg, id, ctl->placement_id);
        if (ctl->placement_id < 0) {
            ghostcon_kitty_image_t *img = find_image(kg, id);
            if (img)
                free_image(kg, img);
        }
        kitty_ack(output_fn, userdata, quiet, id, ctl->placement_id, "OK");
        return;
    }

    kitty_ack(output_fn, userdata, quiet, 0, -1, "error=EINVAL:unsupported delete mode");
}

const ghostcon_kitty_image_t *
ghostcon_kitty_graphics_find_image(const ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    for (int i = 0; i < GHOSTCON_KITTY_MAX_IMAGES; i++) {
        const ghostcon_kitty_image_t *img = &kg->images[i];
        if (img->in_use && img->id == id && img->pixels)
            return img;
    }
    return NULL;
}

void
ghostcon_kitty_graphics_handle(ghostcon_kitty_graphics_t *kg,
                               const char *body, size_t body_len,
                               int32_t cursor_col, int32_t cursor_row,
                               int32_t cell_w, int32_t cell_h,
                               ghostcon_kitty_output_fn output_fn,
                               void *output_userdata,
                               ghostcon_kitty_cursor_move_t *out_move)
{
    if (out_move)
        *out_move = (ghostcon_kitty_cursor_move_t){0};

    if (body_len == 0 || body[0] != 'G')
        return; /* not a Kitty graphics command */
    body++;
    body_len--;

    /* Split control data from payload at the first ';'. */
    const char *semi = memchr(body, ';', body_len);
    size_t control_len = semi ? (size_t)(semi - body) : body_len;
    const char *payload = semi ? semi + 1 : NULL;
    size_t payload_len = semi ? body_len - control_len - 1 : 0;

    /* strtok_r mutates, so copy the control-data segment onto the
       stack (it's bounded -- the whole APC command is already capped
       upstream in stream.c). */
    char control_copy[512];
    if (control_len >= sizeof control_copy)
        control_len = sizeof control_copy - 1;
    memcpy(control_copy, body, control_len);
    control_copy[control_len] = '\0';

    kitty_control_t ctl = kitty_parse_control(control_copy);
    if (!ctl.valid) {
        kitty_ack(output_fn, output_userdata, ctl.quiet >= 0 ? ctl.quiet : 0,
                 ctl.image_id > 0 ? (uint32_t)ctl.image_id : 0, ctl.placement_id,
                 "error=EINVAL:malformed control data");
        return;
    }

    char action = ctl.action ? ctl.action : 't';
    switch (action) {
    case 't':
        handle_transmit(kg, &ctl, payload, payload_len, false,
                        cursor_col, cursor_row, cell_w, cell_h,
                        output_fn, output_userdata, out_move);
        break;
    case 'T':
        handle_transmit(kg, &ctl, payload, payload_len, true,
                        cursor_col, cursor_row, cell_w, cell_h,
                        output_fn, output_userdata, out_move);
        break;
    case 'p':
        handle_placement(kg, &ctl, cursor_col, cursor_row, cell_w, cell_h,
                         output_fn, output_userdata, out_move);
        break;
    case 'd':
        handle_delete(kg, &ctl, output_fn, output_userdata);
        break;
    default:
        kitty_ack(output_fn, output_userdata, ctl.quiet >= 0 ? ctl.quiet : 0,
                 ctl.image_id > 0 ? (uint32_t)ctl.image_id : 0, ctl.placement_id,
                 "error=EINVAL:unsupported action");
        break;
    }
}
