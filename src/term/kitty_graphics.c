#define _DEFAULT_SOURCE /* strtok_r() under -std=c11 without this */

#include "ghostcon/term/kitty_graphics.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* PNG decode (f=100)
 *
 * delegates to libpng rather than hand-rolling inflate/chunk parsing: PNG is a
 * compressed, chunked format, and writing a safe decoder for untrusted PNG data
 * is a much bigger and more dangerous undertaking than this v1's bounds-checked
 * raw-pixel path.
 *
 * Real precedent for not hand-rolling this: Ghostty itself delegates PNG decode
 * to Wuffs (a memory-safety-audited decoder) rather than parsing it in
 * Terminal.zig, and can be built without PNG support at all (sys.decode_png ==
 * null is a first-class, gracefully-handled state there, not a hack)
 *
 * same posture this takes with libpng.
 */

/* Decodes PNG bytes into freshly allocated RGBA pixels, or NULL on any failure
 * (corrupt data, unsupported PNG features, dimensions past this module's limits).
 *
 * Uses libpng's "simplified API" (png_image_begin_read_from_memory), which parses
 * just the header and reports image.width/height BEFORE finish_read() allocates
 * or decodes any pixel data
 *
 * so a malicious PNG that declares an enormous width/height gets rejected here,
 * before any attacker-influenced-size allocation happens, not just after decoding
 * completes (the same "reject before allocating" property Ghostty's own decoder
 * gets from a bounded allocator).
 */
static uint8_t *decode_png_rgba(const uint8_t *data, size_t len, int32_t *out_w, int32_t *out_h)
{
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, len))
        return NULL;

    if (image.width == 0 || image.height == 0 ||
        image.width > GC_KITTY_MAX_DIM || image.height > GC_KITTY_MAX_DIM)
    {
        png_image_free(&image);
        return NULL;
    }

    image.format = PNG_FORMAT_RGBA;
    size_t stride = PNG_IMAGE_ROW_STRIDE(image);
    size_t total = PNG_IMAGE_BUFFER_SIZE(image, stride);
    if (total == 0 || total > GC_KITTY_MAX_IMAGE_BYTES)
    {
        png_image_free(&image);
        return NULL;
    }

    uint8_t *buf = malloc(total);
    if (!buf)
    {
        png_image_free(&image);
        return NULL;
    }

    if (!png_image_finish_read(&image, NULL, buf, (png_int_32)stride, NULL))
    {
        free(buf);
        png_image_free(&image);
        return NULL;
    }

    *out_w = (int32_t)image.width;
    *out_h = (int32_t)image.height;
    png_image_free(&image);
    return buf;
}

/* base64 decode
 *
 * bounded, rejects invalid input rather than skipping it (silently accepting
 * garbage as "decoded fine" would make corrupt/adversarial payloads look like
 * valid images).
 */

static int b64_val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes in place into *out (caller-allocated, at least
 * b64_decoded_max(len) bytes). Returns decoded length, or SIZE_MAX on any
 * invalid character / malformed padding.
 */
static size_t b64_decode(const char *in, size_t len, uint8_t *out, size_t out_cap)
{
    size_t o = 0;
    size_t i = 0;
    while (i < len)
    {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++)
        {
            if (i >= len)
                return SIZE_MAX; /* truncated group */
            uint8_t c = (uint8_t)in[i++];
            if (c == '=')
            {
                v[k] = 0;
                pad++;
            }
            else
            {
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
        for (int k = 0; k < nbytes; k++)
        {
            if (o >= out_cap)
                return SIZE_MAX;
            out[o++] = (uint8_t)(triple >> (16 - 8 * k));
        }
        if (pad)
            break; /* padding only valid on the final group */
    }
    return o;
}

static size_t b64_decoded_max(size_t len)
{
    return ((len + 3) / 4) * 3;
}

/* Shared-memory transmission (t=s) */

/* Reads a POSIX shared-memory segment by name
 *
 * the Kitty t=s medium.
 *
 * Deliberately narrower than t=f/t=t (arbitrary filesystem paths, still deferred
 *
 * see this file's own top-of-header doc comment): shm_open() is confined to the
 * kernel's separate named-shared-memory namespace, never the general filesystem,
 * and this always opens O_RDONLY with no O_CREAT, so ghostcon can only read a
 * segment the client already created
 *
 * it can never create or clobber one.
 *
 * Matches Ghostty's own graphics_image.zig readSharedMemory(): reject before
 * mmap'ing anything if the segment is bigger than max_len (same "reject before
 * allocating on attacker-influenced size" posture as decode_png_rgba above), and
 * unconditionally shm_unlink() the segment afterward
 *
 * success or failure
 *
 * since per the Kitty spec the client creates+writes it and hands ownership of
 * deleting it to the terminal; leaving that to the client would accumulate an
 * unbounded number of live shm segments across repeated transmissions.
 *
 * Returns a freshly malloc'd copy of the segment's contents (caller owns it, the
 * mapping itself is never handed back) on success, NULL on any failure.
 */
static uint8_t *read_shm_segment(const char *name, size_t max_len, size_t *out_len)
{
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max_len)
    {
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        shm_unlink(name);
        return NULL;
    }

    uint8_t *buf = malloc(len);
    if (buf)
        memcpy(buf, map, len);
    munmap(map, len);
    shm_unlink(name);

    if (!buf)
        return NULL;
    *out_len = len;
    return buf;
}

static bool path_in_dir(const char *dir, const char *path)
{
    size_t dir_len = strlen(dir);
    if (dir_len == 0 || strncmp(path, dir, dir_len) != 0)
        return false;
    if (path[dir_len] == '\0' || dir[dir_len - 1] == '/')
        return true;
    return path[dir_len] == '/';
}

static bool read_full(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false; /* file shrank out from under us */
        got += (size_t)n;
    }
    return true;
}

/* Reads a file transmitted via the Kitty t=t (temporary file) or t=f (plain file)
 * medium.
 *
 * Shared implementation
 *
 * the two mediums only differ in whether the filename must match Kitty's own
 * temp-file naming convention and whether the file gets deleted afterward (see
 * the two thin wrappers below), not in how the path itself gets validated:
 *
 * - Opens the client-supplied path FIRST, and only resolves/validates its
 *   real path (via /proc/self/fd, which reflects the actual open inode)
 *   AFTER that -- validating the path string before opening it would leave
 *   a gap where a cooperating process could swap a symlink or directory
 *   entry between check and open (TOCTOU); this order matches Ghostty's own
 *   readFile()/validatedFilePath() (graphics_image.zig), itself a port of
 *   Kitty's reference terminal's approach. - Rejects /proc/ and /sys/
 *   outright (kernel/process internals that were never meant to be
 *   reachable through an "image file" primitive) and /dev/ except /dev/shm/
 *   (POSIX shared memory --read_shm_segment above already handles that
 *   medium safely on its own, dedicated path; there's no reason to let this
 *   reach it too, but Kitty's own reference implementation allows it, so
 *   this matches that rather than being stricter for no interoperability
 *   benefit). - Requires the real path to actually be under /tmp or
 *   /dev/shm, regardless of medium -- ghostcon (unlike Ghostty/Kitty, which
 *   let t=f point at any readable path, gated behind an explicit opt-in
 *   config flag off by default) treats t=f as scoped to the same bounded
 *   locations as t=t rather than genuinely arbitrary, so there's no
 *   unrestricted-filesystem-read mode to gate behind a flag in the first
 *   place.
 */
static uint8_t *read_local_file(const char *client_path, size_t max_len, size_t *out_len, bool require_temp_naming, bool delete_after)
{
    int fd = open(client_path, O_RDONLY);
    if (fd < 0)
        return NULL;

    char fd_path[64];
    snprintf(fd_path, sizeof fd_path, "/proc/self/fd/%d", fd);
    char real_path[4096];
    ssize_t real_len = readlink(fd_path, real_path, sizeof(real_path) - 1);
    if (real_len <= 0)
    {
        close(fd);
        return NULL;
    }
    real_path[real_len] = '\0';

    if (!strncmp(real_path, "/proc/", 6) ||
        !strncmp(real_path, "/sys/", 5) ||
        (!strncmp(real_path, "/dev/", 5) && strncmp(real_path, "/dev/shm/", 9) != 0))
    {
        close(fd);
        return NULL;
    }

    bool in_temp_dir = path_in_dir("/tmp", real_path) || path_in_dir("/dev/shm", real_path);
    if (!in_temp_dir)
    {
        close(fd);
        return NULL;
    }
    if (require_temp_naming)
    {
        /* t=t only: Kitty's own naming convention for files it creates for this
         * exact purpose.
         *
         * Without this, "must be under /tmp" alone would let a hostile client ask
         * the terminal to read+DELETE any other unrelated file that happens to
         * already live in /tmp (another process's lockfile, a cache entry, ...);
         * requiring the convention name means a deleting read can only ever act
         * on a file created FOR this handoff. t=f never deletes (see delete_after
         * below), so it skips this
         *
         * worst case there is reading, not destroying, an unrelated file in that
         * directory.
         */
        const char *base = strrchr(real_path, '/');
        base = base ? base + 1 : real_path;
        if (!strstr(base, "tty-graphics-protocol"))
        {
            close(fd);
            return NULL;
        }
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (size_t)st.st_size > max_len)
    {
        close(fd);
        if (delete_after)
            unlink(real_path);
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    uint8_t *buf = malloc(len);
    bool ok = buf && read_full(fd, buf, len);
    close(fd);
    if (delete_after)
        unlink(real_path);

    if (!ok)
    {
        free(buf);
        return NULL;
    }
    *out_len = len;
    return buf;
}

/* t=t (temporary file): requires Kitty's "tty-graphics-protocol" filename
 * convention, and always unlink()s the file afterward --success or failure
 *
 * exactly like read_shm_segment()'s shm_unlink(): the client creates the file and
 * hands ownership of deleting it to the terminal, so leaving cleanup to the
 * client would accumulate temp files across repeated transmissions the same way
 * not unlinking shm segments would.
 */
static uint8_t *read_temp_file(const char *client_path, size_t max_len, size_t *out_len)
{
    return read_local_file(client_path, max_len, out_len, true, true);
}

/* t=f (plain file): no filename convention required, and never deleted --treated
 * as a read-only cache the client expects to still exist afterward (e.g.
 * re-displaying the same pre-rendered image again later) rather than a one-shot
 * handoff like t=t.
 *
 * Still bounded to /tmp or /dev/shm (see read_local_file()'s own doc comment on
 * why that's not the genuinely-arbitrary-path t=f the Kitty spec itself defines).
 */
static uint8_t *read_cache_file(const char *client_path, size_t max_len, size_t *out_len)
{
    return read_local_file(client_path, max_len, out_len, false, false);
}

/* Control-data (key=value,key=value,...) parsing */

typedef struct
{
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

/* Parses a signed decimal integer, rejecting anything that isn't exactly
 * digits (with an optional leading '-'). Returns false on any malformed
 * value rather than silently truncating/clamping.
 */
static bool parse_i64(const char *s, size_t len, int64_t *out)
{
    if (len == 0)
        return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-')
    {
        neg = true;
        i = 1;
        if (len == 1)
            return false;
    }
    int64_t v = 0;
    for (; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
        if (v > (INT64_MAX - 9) / 10)
            return false; /* overflow guard */
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

/* How a control key's value is extracted.
 *
 * CHAR keys carry a single character (the action, the medium, the delete
 * mode); I32/I64 keys carry a signed decimal integer.
 */
typedef enum
{
    KITTY_VALUE_CHAR,
    KITTY_VALUE_I32,
    KITTY_VALUE_I64,
} kitty_value_kind_t;

/* One row per recognized control key.
 *
 * key     the single character in front of '='
 * kind    how the value is extracted
 * min_len shortest accepted value (character kinds only)
 * max_len longest accepted value (character kinds only)
 * offset  byte offset of the destination field inside kitty_control_t
 *
 * The table replaced a 17-arm strcmp() chain, where every new key meant another
 * branch in a function that was already the second-worst hotspot in the tree --
 * and where a key whose value did not parse fell through to the next arm instead
 * of being rejected, which is exactly the kind of silent misdispatch a table
 * makes obvious.
 */
typedef struct
{
    char               key;
    kitty_value_kind_t kind;
    size_t             min_len;
    size_t             max_len;
    size_t             offset;
} kitty_control_key_t;

static const kitty_control_key_t kitty_control_keys[] = {
    { 'a', KITTY_VALUE_CHAR, 1, 1, offsetof(kitty_control_t, action) },
    { 't', KITTY_VALUE_CHAR, 1, 1, offsetof(kitty_control_t, medium) },
    { 'd', KITTY_VALUE_CHAR, 1, SIZE_MAX, offsetof(kitty_control_t, delete_mode) },
    { 'f', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, format) },
    { 's', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, width) },
    { 'v', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, height) },
    { 'i', KITTY_VALUE_I64, 0, 0, offsetof(kitty_control_t, image_id) },
    { 'p', KITTY_VALUE_I64, 0, 0, offsetof(kitty_control_t, placement_id) },
    { 'm', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, more) },
    { 'q', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, quiet) },
    { 'z', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, z) },
    { 'x', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, crop_x) },
    { 'y', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, crop_y) },
    { 'w', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, crop_w) },
    { 'h', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, crop_h) },
    { 'c', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, cell_cols) },
    { 'r', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, cell_rows) },
    { 'C', KITTY_VALUE_I32, 0, 0, offsetof(kitty_control_t, no_cursor_move) },
};

#define KITTY_CONTROL_KEY_COUNT (sizeof kitty_control_keys / sizeof kitty_control_keys[0])

/* Stores a value into the field the table row points at.
 *
 * memcpy rather than a typed pointer store, so the row's offset stays the
 * single source of truth for the field layout.
 */
static void kitty_control_store(kitty_control_t *c, const kitty_control_key_t *row, const char *val, size_t val_len)
{
    switch (row->kind)
    {
    case KITTY_VALUE_CHAR:
        if (val_len < row->min_len || val_len > row->max_len)
            return;
        memcpy((char *)c + row->offset, val, 1);
        return;
    case KITTY_VALUE_I32:
    {
        int64_t iv = 0;
        if (!parse_i64(val, val_len, &iv))
            return;
        int32_t v = (int32_t)iv;
        memcpy((char *)c + row->offset, &v, sizeof v);
        return;
    }
    case KITTY_VALUE_I64:
    {
        int64_t iv = 0;
        if (!parse_i64(val, val_len, &iv))
            return;
        memcpy((char *)c + row->offset, &iv, sizeof iv);
        return;
    }
    }
}

/* Finds the table row for a single-character key. */
static const kitty_control_key_t
*kitty_control_key_find(char key)
{
    for (size_t i = 0; i < KITTY_CONTROL_KEY_COUNT; i++)
    {
        if (kitty_control_keys[i].key == key)
            return &kitty_control_keys[i];
    }
    return NULL;
}

/* control is the substring before the payload's ';' (or the whole body if
 * there is no payload section). Mutates a scratch copy, so the caller must
 * pass a writable, NUL-terminated buffer.
 */
static kitty_control_t
kitty_parse_control(char *control)
{
    /* Every field's "absent" sentinel is spelled out here rather than assigned
     * one line at a time below.
     *
     * -1 is the sentinel for every field except z, which legitimately ranges
     * negative (spec: z<0 renders behind text) -- -1 collides with a real value a
     * client would actually send (it's the most common "just behind text"
     * choice), so it can't double as the "absent" sentinel the way -1 safely does
     * for every other field here (all of which are spec-defined non-negative).
     *
     * Found live: every placement sent with z=-1 was silently coerced to the z=0
     * default by the old `ctl->z >= 0 ? ctl->z : 0` check below, so a z<0
     * (behind-text) placement was actually rendering in the z>=0
     * (in-front-of-text) bucket the entire time
     *
     * an adversarial two-negative-z ordering test caught it, not the original
     * zindex test page, which never actually verified genuine text-over-image
     * compositing, only that both colors existed somewhere on screen.
     */
    kitty_control_t c = {
        .format = -1,
        .width = -1,
        .height = -1,
        .image_id = -1,
        .placement_id = -1,
        .more = -1,
        .quiet = -1,
        .z = INT32_MIN,
        .crop_x = -1,
        .crop_y = -1,
        .crop_w = -1,
        .crop_h = -1,
        .cell_cols = -1,
        .cell_rows = -1,
        .no_cursor_move = -1,
        .valid = true,
    };

    char *save = NULL;
    for (char *tok = strtok_r(control, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save))
    {
        char *eq = strchr(tok, '=');
        if (!eq)
        {
            c.valid = false;
            continue;
        }
        *eq = '\0';
        const char *key = tok;
        const char *val = eq + 1;
        size_t val_len = strlen(val);

        /* Every recognized key is a single character; anything longer is
         * unknown and ignored.
         */
        if (key[0] == '\0' || key[1] != '\0')
            continue;
        const kitty_control_key_t *row = kitty_control_key_find(key[0]);
        if (!row)
        {
            /* Unrecognized keys (I=, o=, etc.) are accepted and ignored
             * --forward-compatible with clients that send extra hints this v1
             * doesn't act on.
             */
            continue;
        }
        kitty_control_store(&c, row, val, val_len);
    }
    return c;
}

/* The transmission medium (the t= control key).
 *
 * DIRECT carries the image bytes in the escape sequence itself
 *
 * the other three carry the name of a shared-memory segment or a file,
 * base64-encoded.
 */
typedef enum
{
    KITTY_MEDIUM_DIRECT,
    KITTY_MEDIUM_SHM,
    KITTY_MEDIUM_TEMP_FILE,
    KITTY_MEDIUM_CACHE_FILE,
} kitty_medium_t;

/* Command context
 *
 * One context per APC "G" command, carried through the transmit and placement
 * handlers.
 *
 * It exists because the transmit path has ~20 failure exits that all have to
 * report the same five things (who to write to, how loudly, which image, which
 * placement, and what went wrong).
 *
 * Re-typing those at every exit is how a mismatch between two of them becomes a
 * silent protocol bug rather than a compile error.
 */
typedef struct
{
    ghostcon_kitty_graphics_t    *kg;
    const kitty_control_t        *ctl;
    ghostcon_kitty_output_fn      output_fn;
    void                        *userdata;
    ghostcon_kitty_cursor_move_t *out_move;
    int32_t cursor_col, cursor_row, cell_w, cell_h;

    /* Ack target.
     *
     * image_id 0 means "no response at all" (see kitty_ack).
     */
    int32_t  quiet;
    uint32_t image_id;
    int64_t  placement_id;

    /* The transmission medium (t= key). */
    kitty_medium_t medium;

    /* Transfer state, filled in as the command is resolved. */
    bool     also_display;
    bool     more;
    bool     continuing;
    int64_t  resolved_id;
    bool     resolved_display;
    int32_t  resolved_z;
    int32_t  resolved_crop_x, resolved_crop_y, resolved_crop_w, resolved_crop_h;
    int32_t  resolved_cell_cols, resolved_cell_rows;
    int32_t  resolved_no_cursor_move;
    int64_t  resolved_placement_id;
    int32_t  bpp;
    bool     is_png;
    int32_t  width, height;
    size_t   expected_len;

    /* The buffer this command owns, released by kitty_fail() unless it is
     * handed over to the image slot.
     */
    uint8_t *pending;
    size_t   pending_len;

    /* The image slot this command owns, released by kitty_fail(). */
    ghostcon_kitty_image_t *img;
    bool release_img;
    bool clear_active;

    /* The failure status, set by whichever step rejected the command. */
    const char *error;
} kitty_cmd_t;

/* Image store */

static ghostcon_kitty_image_t
*find_image(ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    for (int i = 0; i < GC_KITTY_MAX_IMAGES; i++)
        if (kg->images[i].in_use && kg->images[i].id == id)
            return &kg->images[i];
    return NULL;
}

static void free_image(ghostcon_kitty_graphics_t *kg, ghostcon_kitty_image_t *img)
{
    if (!img->in_use)
        return;
    if (img->pixels)
    {
        kg->total_bytes -= img->pixel_len;
        free(img->pixels);
    }
    free(img->recv_buf);
    memset(img, 0, sizeof *img);
}

static ghostcon_kitty_image_t
*alloc_image_slot(ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    ghostcon_kitty_image_t *existing = find_image(kg, id);
    if (existing)
    {
        free_image(kg, existing);
        existing->in_use = true;
        existing->id = id;
        return existing;
    }
    for (int i = 0; i < GC_KITTY_MAX_IMAGES; i++)
    {
        if (!kg->images[i].in_use)
        {
            kg->images[i].in_use = true;
            kg->images[i].id = id;
            return &kg->images[i];
        }
    }
    return NULL; /* ENOSPC */
}

static void delete_placements_for_image(ghostcon_kitty_graphics_t *kg, uint32_t image_id, int64_t placement_id /* -1 = all placements of this image */)
{
    for (int i = 0; i < GC_KITTY_MAX_PLACEMENTS; i++)
    {
        ghostcon_kitty_placement_t *p = &kg->placements[i];
        if (!p->in_use || p->image_id != image_id)
            continue;
        if (placement_id >= 0 && p->placement_id != (uint32_t)placement_id)
            continue;
        p->in_use = false;
    }
}

static ghostcon_kitty_placement_t
*alloc_placement_slot(ghostcon_kitty_graphics_t *kg, uint32_t image_id, uint32_t placement_id)
{
    for (int i = 0; i < GC_KITTY_MAX_PLACEMENTS; i++)
    {
        if (kg->placements[i].in_use &&
            kg->placements[i].image_id == image_id &&
            kg->placements[i].placement_id == placement_id)
        {
            return &kg->placements[i]; /* replace existing placement */
        }
    }
    for (int i = 0; i < GC_KITTY_MAX_PLACEMENTS; i++)
    {
        if (!kg->placements[i].in_use)
        {
            memset(&kg->placements[i], 0, sizeof kg->placements[i]);
            kg->placements[i].in_use = true;
            return &kg->placements[i];
        }
    }
    return NULL; /* ENOSPC */
}

/* Acks */

static void kitty_ack(const kitty_cmd_t *cmd, const char *status /* "OK" or "error=CODE:message" */)
{
    if (!cmd->output_fn)
        return;
    /* Matches Ghostty's Response.encode() exactly: no id (and this v1 doesn't
     * implement I=/image-number at all) means there's nothing for the client to
     * attach a response to, so the terminal stays silent --even for an error.
     *
     * A malformed command with no i= at all (e.g. missing i= entirely)
     * legitimately gets no reply on real Ghostty; echoing "i=0;error=..." back,
     * as an earlier version of this function did unconditionally, is protocol
     * noise a well-behaved client isn't expecting.
     */
    if (cmd->image_id == 0)
        return;
    /* q=1 suppresses OK acks (errors still sent); q=2 suppresses all. */
    bool is_error = strncmp(status, "error", 5) == 0;
    if (cmd->quiet >= 2)
        return;
    if (cmd->quiet >= 1 && !is_error)
        return;

    char buf[192];
    int n;
    if (cmd->placement_id > 0)
    {
        n = snprintf(buf, sizeof buf, "\x1b_Gi=%u,p=%lld;%s\x1b\\",
                     cmd->image_id, (long long)cmd->placement_id, status);
    }
    else
    {
        n = snprintf(buf, sizeof buf, "\x1b_Gi=%u;%s\x1b\\", cmd->image_id, status);
    }
    if (n > 0)
        cmd->output_fn(cmd->userdata, (const uint8_t *)buf, (size_t)n);
}

/* Releases everything the command owns and reports the failure.
 *
 * Every failure exit in the transmit path is one line:
 *
 *     return kitty_fail(cmd, "error=...");
 *
 * The buffer and the image slot the command took ownership of are released
 * here, so a failing command cannot leak either one -- the two things that
 * actually leak memory in a terminal that reads images off the filesystem.
 */
static void kitty_fail(kitty_cmd_t *cmd, const char *status)
{
    if (cmd->pending)
    {
        free(cmd->pending);
        cmd->pending = NULL;
        cmd->pending_len = 0;
    }
    if (cmd->release_img && cmd->img)
    {
        free_image(cmd->kg, cmd->img);
        cmd->img = NULL;
        cmd->release_img = false;
    }
    if (cmd->clear_active)
        cmd->kg->active_transfer_id = -1;
    kitty_ack(cmd, status);
}

/* Lifecycle */

void ghostcon_kitty_graphics_init(ghostcon_kitty_graphics_t *kg)
{
    memset(kg, 0, sizeof *kg);
    kg->active_transfer_id = -1;
    kg->active_transfer_z = INT32_MIN;
    kg->active_transfer_crop_x = -1;
    kg->active_transfer_crop_y = -1;
    kg->active_transfer_crop_w = -1;
    kg->active_transfer_crop_h = -1;
    kg->active_transfer_cell_cols = -1;
    kg->active_transfer_cell_rows = -1;
    kg->active_transfer_no_cursor_move = -1;
    kg->active_transfer_placement_id = -1;
}

void ghostcon_kitty_graphics_deinit(ghostcon_kitty_graphics_t *kg)
{
    for (int i = 0; i < GC_KITTY_MAX_IMAGES; i++)
    {
        free(kg->images[i].pixels);
        free(kg->images[i].recv_buf);
    }
    memset(kg, 0, sizeof *kg);
}

/* Command handling */

/* Matches Ghostty's kitty/graphics_exec.zig cursor_movement==.after (the default
 *
 * only C=1 suppresses it): advance past the placement's cell footprint, computed
 * from its actual drawn pixel size (draw_w_px/draw_h_px, already resolved by the
 * caller from crop/c=/r= the same way render/machine.c's render_one_placement
 * resolves them for drawing) divided by the renderer's current cell size.
 */
static void compute_cursor_move(int32_t no_cursor_move, int32_t draw_w_px, int32_t draw_h_px, int32_t anchor_col, int32_t cell_w, int32_t cell_h, ghostcon_kitty_cursor_move_t *out_move)
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

/* Resolves the image id and the placement-affecting keys for this chunk.
 *
 * A continuation chunk (m=1 was sent previously, this chunk lacking its own
 * i=/a=) resolves back to whichever transfer is currently in progress --see the
 * struct's own doc comment on why this can't just require i= on every chunk.
 *
 * Returns false if the command cannot proceed
 *
 * the caller acks.
 */
static bool kitty_resolve_transfer(kitty_cmd_t *cmd)
{
    const kitty_control_t *ctl = cmd->ctl;
    cmd->resolved_id = ctl->image_id;
    cmd->resolved_display = cmd->also_display;
    /* Placement-affecting keys that are only meaningful/sent on the first chunk
     *
     * default to this chunk's own (usually absent) values, overridden below from
     * the saved snapshot only when resolving back to an in-progress transfer.
     */
    cmd->resolved_z = ctl->z;
    cmd->resolved_crop_x = ctl->crop_x;
    cmd->resolved_crop_y = ctl->crop_y;
    cmd->resolved_crop_w = ctl->crop_w;
    cmd->resolved_crop_h = ctl->crop_h;
    cmd->resolved_cell_cols = ctl->cell_cols;
    cmd->resolved_cell_rows = ctl->cell_rows;
    cmd->resolved_no_cursor_move = ctl->no_cursor_move;
    cmd->resolved_placement_id = ctl->placement_id;

    if (cmd->resolved_id < 0)
    {
        if (cmd->kg->active_transfer_id < 0)
        {
            /* No image id and no transfer to continue: the ack carries no id,
             * so the terminal stays silent (see kitty_ack).
             */
            cmd->image_id = 0;
            cmd->placement_id = -1;
            return false;
        }
        cmd->resolved_id = cmd->kg->active_transfer_id;
        cmd->resolved_display = cmd->kg->active_transfer_display;
        cmd->resolved_z = cmd->kg->active_transfer_z;
        cmd->resolved_crop_x = cmd->kg->active_transfer_crop_x;
        cmd->resolved_crop_y = cmd->kg->active_transfer_crop_y;
        cmd->resolved_crop_w = cmd->kg->active_transfer_crop_w;
        cmd->resolved_crop_h = cmd->kg->active_transfer_crop_h;
        cmd->resolved_cell_cols = cmd->kg->active_transfer_cell_cols;
        cmd->resolved_cell_rows = cmd->kg->active_transfer_cell_rows;
        cmd->resolved_no_cursor_move = cmd->kg->active_transfer_no_cursor_move;
        cmd->resolved_placement_id = cmd->kg->active_transfer_placement_id;
    }
    if (cmd->resolved_id > UINT32_MAX)
    {
        cmd->image_id = 0;
        cmd->placement_id = -1;
        return false;
    }
    cmd->image_id = (uint32_t)cmd->resolved_id;
    cmd->also_display = cmd->resolved_display;
    return true;
}

/* Resolves the transmission medium (t=) and rejects the combinations that
 * cannot work.
 */
static bool kitty_resolve_medium(kitty_cmd_t *cmd)
{
    switch (cmd->ctl->medium)
    {
    case 0:
    case 'd':
        cmd->medium = KITTY_MEDIUM_DIRECT;
        break;
    case 's':
        cmd->medium = KITTY_MEDIUM_SHM;
        break;
    case 't':
        cmd->medium = KITTY_MEDIUM_TEMP_FILE;
        break;
    case 'f':
        cmd->medium = KITTY_MEDIUM_CACHE_FILE;
        break;
    default:
        cmd->error = "error=EBADF:unsupported transmission medium";
        return false;
    }
    if (cmd->medium != KITTY_MEDIUM_DIRECT && cmd->ctl->more == 1)
    {
        /* Chunked path/name assembly is legal per spec (a path is just another
         * base64 payload, mechanically chunkable like anything else) but never
         * useful in practice
         *
         * shm segment names and file paths are always short enough for one chunk
         *
         * and adding it would mean threading a second "this accumulation is a
         * path, not pixels" mode through the entire chunk/continuation machinery
         * above.
         *
         * Reject cleanly rather than silently mishandling it.
         */
        cmd->error = "error=EBADF:chunked shared-memory/temp-file transmission is not supported";
        return false;
    }
    return true;
}

/* Determines the pixel format of the transfer: raw RGB/RGBA or PNG.
 *
 * f= (like s=/v=/i= below) is only meaningful on the first chunk of a
 * multi-chunk transfer
 *
 * continuation chunks legally omit it, so re-deriving bpp from ctl->format on
 * every chunk would silently default to 32 (RGBA) instead of the format
 * actually agreed on in chunk 1, corrupting the declared size for every later
 * chunk.
 *
 * PNG (f=100) is tracked the same way, via img->bpp == 0 (raw formats are
 * always 3 or 4, so 0 is unambiguous) rather than a separate flag on the
 * image struct.
 */
static bool kitty_resolve_format(kitty_cmd_t *cmd)
{
    if (cmd->continuing)
    {
        cmd->bpp = cmd->img->bpp;
        cmd->is_png = (cmd->bpp == 0);
        return true;
    }
    int32_t fmt = cmd->ctl->format >= 0 ? cmd->ctl->format : 32;
    cmd->is_png = (fmt == 100);
    if (cmd->is_png)
    {
        cmd->bpp = 0; /* unknown until decode -- PNG carries its own dimensions */
        return true;
    }
    cmd->bpp = (fmt == 24) ? 3 : (fmt == 32) ? 4 : -1;
    if (cmd->bpp < 0)
    {
        cmd->error = "error=EBADF:unsupported format";
        return false;
    }
    return true;
}

/* Raw formats: width/height come from s=/v= and expected_len is the exact decoded
 * pixel size.
 *
 * PNG: dimensions come from the decoded PNG itself (Ghostty ignores s=/v= for
 * f=100 the same way --graphics_command.zig's width/height table maps .png to
 * null), so there's nothing to validate here yet; expected_len instead bounds how
 * many raw (still-compressed) bytes this transfer may accumulate before decode,
 * same cap as everything else in this module, matching Ghostty's own pre-decode
 * size cap on the still-compressed data (graphics_image.zig's "image data too
 * large" check, `data.items.len + data.len > max_size`).
 */
static bool kitty_resolve_dimensions(kitty_cmd_t *cmd)
{
    if (cmd->is_png)
    {
        cmd->width = 0;
        cmd->height = 0;
        cmd->expected_len = GC_KITTY_MAX_IMAGE_BYTES;
        return true;
    }
    if (cmd->continuing)
    {
        cmd->width = cmd->img->width;
        cmd->height = cmd->img->height;
    }
    else
    {
        cmd->width = cmd->ctl->width;
        cmd->height = cmd->ctl->height;
        if (cmd->width <= 0 || cmd->height <= 0 ||
            (uint32_t)cmd->width > GC_KITTY_MAX_DIM ||
            (uint32_t)cmd->height > GC_KITTY_MAX_DIM)
        {
            cmd->error = "error=EINVAL:missing or out-of-range s=/v=";
            return false;
        }
    }
    cmd->expected_len = (size_t)cmd->width * (size_t)cmd->height * (size_t)cmd->bpp;
    if (cmd->expected_len > GC_KITTY_MAX_IMAGE_BYTES)
    {
        cmd->error = "error=EINVAL:image too large";
        return false;
    }
    return true;
}

/* Reads the transfer's bytes into the buffer the command owns.
 *
 * For the file and shared-memory media the payload is the base64-encoded
 * segment name or file PATH (per spec, the payload is always base64
 * regardless of medium
 *
 * only what it MEANS differs), not image bytes.
 */
static bool kitty_fetch_payload(kitty_cmd_t *cmd, const char *payload, size_t payload_len)
{
    if (cmd->medium == KITTY_MEDIUM_DIRECT)
    {
        size_t cap = b64_decoded_max(payload_len);
        uint8_t *buf = cap ? malloc(cap) : NULL;
        if (cap && !buf)
        {
            cmd->error = "error=ENOMEM:allocation failed";
            return false;
        }
        size_t len = payload_len ? b64_decode(payload, payload_len, buf, cap) : 0;
        if (len == SIZE_MAX)
        {
            free(buf);
            cmd->error = "error=EINVAL:malformed base64 payload";
            return false;
        }
        cmd->pending = buf;
        cmd->pending_len = len;
        return true;
    }

    size_t name_cap = b64_decoded_max(payload_len);
    uint8_t *name_buf = malloc(name_cap + 1);
    if (!name_buf)
    {
        cmd->error = "error=ENOMEM:allocation failed";
        return false;
    }
    size_t name_len = payload_len ? b64_decode(payload, payload_len, name_buf, name_cap) : 0;
    if (name_len == SIZE_MAX)
    {
        free(name_buf);
        cmd->error = "error=EINVAL:malformed base64 payload";
        return false;
    }
    name_buf[name_len] = '\0';
    /* POSIX shm names/paths cannot contain internal NULs
     *
     * same check Ghostty applies to the decoded path/name before calling
     * shm_open()/openFile(), and for the same reason (a NUL would silently
     * truncate the string the syscall actually sees).
     */
    if (strlen((char *)name_buf) != name_len)
    {
        free(name_buf);
        cmd->error = "error=EINVAL:invalid shared-memory/file path";
        return false;
    }
    uint8_t *data = NULL;
    size_t data_len = 0;
    if (cmd->medium == KITTY_MEDIUM_SHM)
        data = read_shm_segment((char *)name_buf, GC_KITTY_MAX_IMAGE_BYTES, &data_len);
    else if (cmd->medium == KITTY_MEDIUM_TEMP_FILE)
        data = read_temp_file((char *)name_buf, GC_KITTY_MAX_IMAGE_BYTES, &data_len);
    else
        data = read_cache_file((char *)name_buf, GC_KITTY_MAX_IMAGE_BYTES, &data_len);
    free(name_buf);
    if (!data)
    {
        cmd->error = (cmd->medium == KITTY_MEDIUM_SHM)
                         ? "error=EBADF:unable to read shared memory segment"
                         : "error=EBADF:unable to read file";
        return false;
    }
    cmd->pending = data;
    cmd->pending_len = data_len;
    return true;
}

/* Claims the image slot for this transfer, and (for a chunked transfer) the
 * receive buffer the chunks accumulate into.
 */
static bool kitty_begin_transfer(kitty_cmd_t *cmd)
{
    cmd->more = (cmd->ctl->more == 1);
    if (cmd->continuing)
        return true; /* the slot (and its receive buffer) already exist */

    ghostcon_kitty_image_t *img = alloc_image_slot(cmd->kg, cmd->image_id);
    if (!img)
    {
        cmd->error = "error=ENOSPC:too many images";
        return false;
    }
    cmd->img = img;
    cmd->release_img = true;
    img->width = cmd->width;
    img->height = cmd->height;
    img->bpp = cmd->bpp;
    if (!cmd->more)
        return true;

    img->recv_buf = malloc(cmd->expected_len ? cmd->expected_len : 1);
    if (!img->recv_buf)
    {
        cmd->error = "error=ENOMEM:allocation failed";
        return false;
    }
    img->recv_len = 0;
    img->receiving = true;
    cmd->kg->active_transfer_id = (int64_t)cmd->image_id;
    cmd->kg->active_transfer_display = cmd->also_display;
    cmd->kg->active_transfer_z = cmd->resolved_z;
    cmd->kg->active_transfer_crop_x = cmd->resolved_crop_x;
    cmd->kg->active_transfer_crop_y = cmd->resolved_crop_y;
    cmd->kg->active_transfer_crop_w = cmd->resolved_crop_w;
    cmd->kg->active_transfer_crop_h = cmd->resolved_crop_h;
    cmd->kg->active_transfer_cell_cols = cmd->resolved_cell_cols;
    cmd->kg->active_transfer_cell_rows = cmd->resolved_cell_rows;
    cmd->kg->active_transfer_no_cursor_move = cmd->resolved_no_cursor_move;
    cmd->kg->active_transfer_placement_id = cmd->resolved_placement_id;
    return true;
}

/* Appends one chunk to the transfer's receive buffer. */
static bool kitty_accumulate_chunk(kitty_cmd_t *cmd)
{
    if (cmd->img->recv_len + cmd->pending_len > cmd->expected_len)
    {
        cmd->release_img = true;
        cmd->clear_active = true;
        cmd->error = "error=EINVAL:payload exceeds declared size";
        return false;
    }
    if (cmd->pending && cmd->pending_len > 0)
        memcpy(cmd->img->recv_buf + cmd->img->recv_len, cmd->pending, cmd->pending_len);
    cmd->img->recv_len += cmd->pending_len;
    free(cmd->pending);
    cmd->pending = NULL;
    cmd->pending_len = 0;
    return true;
}

/* Turns the accumulated (or single-shot) payload into the image's pixels.
 *
 * Raw formats require the accumulated size to match the declared width*height*bpp
 * exactly; PNG's compressed size is whatever it happens to be, so there's nothing
 * to match against here --decode_png_rgba is what actually validates it (a
 * truncated or corrupt PNG simply fails to decode).
 */
static bool kitty_finalize_transfer(kitty_cmd_t *cmd)
{
    uint8_t *final_pixels;
    size_t final_len;
    if (cmd->img->receiving)
    {
        if (!cmd->is_png && cmd->img->recv_len + cmd->pending_len != cmd->expected_len)
        {
            cmd->release_img = true;
            cmd->clear_active = true;
            cmd->error = "error=EINVAL:size mismatch";
            return false;
        }
        if (cmd->pending && cmd->pending_len > 0)
            memcpy(cmd->img->recv_buf + cmd->img->recv_len, cmd->pending, cmd->pending_len);
        cmd->img->recv_len += cmd->pending_len;
        free(cmd->pending);
        cmd->pending = NULL;
        cmd->pending_len = 0;
        final_pixels = cmd->img->recv_buf;
        final_len = cmd->is_png ? cmd->img->recv_len : cmd->expected_len;
        cmd->img->recv_buf = NULL;
        cmd->img->receiving = false;
        cmd->kg->active_transfer_id = -1;
    }
    else
    {
        if (!cmd->is_png && cmd->pending_len != cmd->expected_len)
        {
            cmd->release_img = true;
            cmd->error = "error=EINVAL:size mismatch";
            return false;
        }
        final_pixels = cmd->pending;
        final_len = cmd->pending_len;
    }

    if (cmd->is_png)
    {
        int32_t png_w = 0, png_h = 0;
        uint8_t *rgba = decode_png_rgba(final_pixels, final_len, &png_w, &png_h);
        free(final_pixels);
        cmd->pending = NULL;
        cmd->pending_len = 0;
        if (!rgba)
        {
            cmd->release_img = true;
            cmd->error = "error=EINVAL:invalid or unsupported PNG data";
            return false;
        }
        final_pixels = rgba;
        final_len = (size_t)png_w * (size_t)png_h * 4;
        cmd->width = png_w;
        cmd->height = png_h;
        cmd->bpp = 4;
    }

    /* The command owns the finished pixels until the image slot takes them. */
    cmd->pending = final_pixels;
    cmd->pending_len = final_len;

    if (cmd->kg->total_bytes + final_len > GC_KITTY_MAX_TOTAL_BYTES)
    {
        cmd->release_img = true;
        cmd->error = "error=ENOSPC:total image memory limit exceeded";
        return false;
    }

    cmd->img->width = cmd->width;
    cmd->img->height = cmd->height;
    cmd->img->bpp = cmd->bpp;
    cmd->img->pixels = final_pixels;
    cmd->img->pixel_len = final_len;
    cmd->img->generation++;
    cmd->kg->total_bytes += final_len;
    cmd->pending = NULL; /* ownership moved into the image slot */
    cmd->pending_len = 0;
    return true;
}

/* Creates the placement for a transmit-and-display command. */
static bool kitty_place_image(kitty_cmd_t *cmd)
{
    if (!cmd->also_display)
        return true;
    uint32_t placement_id = cmd->resolved_placement_id > 0 ? (uint32_t)cmd->resolved_placement_id : 0;
    ghostcon_kitty_placement_t *p = alloc_placement_slot(cmd->kg, cmd->image_id, placement_id);
    if (!p)
    {
        /* The failure ack names the resolved placement, while the success ack
         * names the placement of this chunk -- for a chunked transfer those
         * differ, since only the first chunk carries p=.
         */
        cmd->placement_id = cmd->resolved_placement_id;
        cmd->error = "error=ENOSPC:too many placements";
        return false;
    }
    p->image_id = cmd->image_id;
    p->placement_id = placement_id;
    p->anchor_col = cmd->cursor_col;
    p->anchor_row = cmd->cursor_row;
    p->z = cmd->resolved_z != INT32_MIN ? cmd->resolved_z : 0;
    p->crop_x = cmd->resolved_crop_x >= 0 ? cmd->resolved_crop_x : 0;
    p->crop_y = cmd->resolved_crop_y >= 0 ? cmd->resolved_crop_y : 0;
    p->crop_w = cmd->resolved_crop_w >= 0 ? cmd->resolved_crop_w : 0;
    p->crop_h = cmd->resolved_crop_h >= 0 ? cmd->resolved_crop_h : 0;
    p->cell_cols = cmd->resolved_cell_cols >= 0 ? cmd->resolved_cell_cols : 0;
    p->cell_rows = cmd->resolved_cell_rows >= 0 ? cmd->resolved_cell_rows : 0;

    int32_t draw_w_px = p->crop_w > 0 ? p->crop_w : cmd->img->width;
    int32_t draw_h_px = p->crop_h > 0 ? p->crop_h : cmd->img->height;
    if (p->cell_cols > 0 && p->cell_rows > 0)
    {
        draw_w_px = p->cell_cols * cmd->cell_w;
        draw_h_px = p->cell_rows * cmd->cell_h;
    }
    compute_cursor_move(cmd->resolved_no_cursor_move, draw_w_px, draw_h_px,
                        p->anchor_col, cmd->cell_w, cmd->cell_h, cmd->out_move);
    return true;
}

/* Transmits an image, or one chunk of one. */
static void handle_transmit(kitty_cmd_t *cmd, const char *payload, size_t payload_len)
{
    if (!kitty_resolve_transfer(cmd) || !kitty_resolve_medium(cmd))
    {
        kitty_fail(cmd, cmd->error);
        return;
    }
    cmd->img = find_image(cmd->kg, cmd->image_id);
    cmd->continuing = cmd->img && cmd->img->receiving;
    if (!kitty_resolve_format(cmd) ||
        !kitty_resolve_dimensions(cmd) ||
        !kitty_fetch_payload(cmd, payload, payload_len) ||
        !kitty_begin_transfer(cmd))
    {
        kitty_fail(cmd, cmd->error);
        return;
    }
    if (cmd->more)
    {
        if (!kitty_accumulate_chunk(cmd))
        {
            kitty_fail(cmd, cmd->error);
            return;
        }
        return; /* no ack for intermediate chunks */
    }
    if (!kitty_finalize_transfer(cmd) || !kitty_place_image(cmd))
    {
        kitty_fail(cmd, cmd->error);
        return;
    }
    kitty_ack(cmd, "OK");
}

static void handle_placement(kitty_cmd_t *cmd)
{
    const kitty_control_t *ctl = cmd->ctl;
    if (ctl->image_id < 0 || ctl->image_id > UINT32_MAX)
    {
        cmd->image_id = 0;
        cmd->placement_id = -1;
        kitty_fail(cmd, "error=EINVAL:missing or invalid i=");
        return;
    }
    cmd->image_id = (uint32_t)ctl->image_id;
    const ghostcon_kitty_image_t *img = find_image(cmd->kg, cmd->image_id);
    if (!img || !img->pixels)
    {
        kitty_fail(cmd, "error=ENOENT:no image with that id");
        return;
    }

    int32_t cx = ctl->crop_x >= 0 ? ctl->crop_x : 0;
    int32_t cy = ctl->crop_y >= 0 ? ctl->crop_y : 0;
    int32_t cw = ctl->crop_w >= 0 ? ctl->crop_w : 0;
    int32_t ch = ctl->crop_h >= 0 ? ctl->crop_h : 0;
    if (cw > 0 && ch > 0)
    {
        if (cx < 0 || cy < 0 || cx + cw > img->width || cy + ch > img->height)
        {
            kitty_fail(cmd, "error=EINVAL:crop rect out of bounds");
            return;
        }
    }

    uint32_t placement_id = ctl->placement_id > 0 ? (uint32_t)ctl->placement_id : 0;
    ghostcon_kitty_placement_t *p = alloc_placement_slot(cmd->kg, cmd->image_id, placement_id);
    if (!p)
    {
        kitty_fail(cmd, "error=ENOSPC:too many placements");
        return;
    }
    p->image_id = cmd->image_id;
    p->placement_id = placement_id;
    p->anchor_col = cmd->cursor_col;
    p->anchor_row = cmd->cursor_row;
    p->z = ctl->z != INT32_MIN ? ctl->z : 0;
    p->crop_x = cx;
    p->crop_y = cy;
    p->crop_w = cw;
    p->crop_h = ch;
    p->cell_cols = ctl->cell_cols >= 0 ? ctl->cell_cols : 0;
    p->cell_rows = ctl->cell_rows >= 0 ? ctl->cell_rows : 0;

    int32_t draw_w_px = cw > 0 ? cw : img->width;
    int32_t draw_h_px = ch > 0 ? ch : img->height;
    if (p->cell_cols > 0 && p->cell_rows > 0)
    {
        draw_w_px = p->cell_cols * cmd->cell_w;
        draw_h_px = p->cell_rows * cmd->cell_h;
    }
    compute_cursor_move(ctl->no_cursor_move, draw_w_px, draw_h_px,
                        p->anchor_col, cmd->cell_w, cmd->cell_h, cmd->out_move);

    kitty_ack(cmd, "OK");
}

static void handle_delete(kitty_cmd_t *cmd)
{
    const kitty_control_t *ctl = cmd->ctl;
    /* Matches Ghostty's graphics_exec.zig delete() exactly: "Delete never
     * responds on success"
     *
     * and, since it unconditionally returns an empty Response regardless of what
     * happened internally, not on failure either.
     *
     * No kitty_ack() calls anywhere in this function is intentional, not an
     * oversight.
     */
    if (ctl->delete_mode == 'a')
    {
        for (int i = 0; i < GC_KITTY_MAX_IMAGES; i++)
            free_image(cmd->kg, &cmd->kg->images[i]);
        memset(cmd->kg->placements, 0, sizeof cmd->kg->placements);
        return;
    }
    if (ctl->delete_mode == 'i')
    {
        if (ctl->image_id < 0 || ctl->image_id > UINT32_MAX)
            return;
        uint32_t id = (uint32_t)ctl->image_id;
        delete_placements_for_image(cmd->kg, id, ctl->placement_id);
        if (ctl->placement_id < 0)
        {
            ghostcon_kitty_image_t *img = find_image(cmd->kg, id);
            if (img)
                free_image(cmd->kg, img);
        }
    }
    /* Unsupported delete modes (per this v1's scope
     *
     * see kitty_graphics.h) are silently no-ops, same as an unrecognized mode is
     * on real Ghostty (it just wouldn't match any Delete union tag).
     */
}

const ghostcon_kitty_image_t
*ghostcon_kitty_graphics_find_image(const ghostcon_kitty_graphics_t *kg, uint32_t id)
{
    for (int i = 0; i < GC_KITTY_MAX_IMAGES; i++)
    {
        const ghostcon_kitty_image_t *img = &kg->images[i];
        if (img->in_use && img->id == id && img->pixels)
            return img;
    }
    return NULL;
}

void ghostcon_kitty_graphics_handle(ghostcon_kitty_graphics_t *kg, const char *body, size_t body_len, int32_t cursor_col, int32_t cursor_row, int32_t cell_w, int32_t cell_h, ghostcon_kitty_output_fn output_fn, void *output_userdata, ghostcon_kitty_cursor_move_t *out_move)
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

    /* strtok_r mutates, so copy the control-data segment onto the stack (it's
     * bounded
     *
     * the whole APC command is already capped upstream in stream.c).
     */
    char control_copy[512];
    if (control_len >= sizeof control_copy)
        control_len = sizeof control_copy - 1;
    memcpy(control_copy, body, control_len);
    control_copy[control_len] = '\0';

    kitty_control_t ctl = kitty_parse_control(control_copy);
    kitty_cmd_t cmd = {
        .kg = kg,
        .ctl = &ctl,
        .output_fn = output_fn,
        .userdata = output_userdata,
        .out_move = out_move,
        .cursor_col = cursor_col,
        .cursor_row = cursor_row,
        .cell_w = cell_w,
        .cell_h = cell_h,
        .quiet = ctl.quiet >= 0 ? ctl.quiet : 0,
        .image_id = ctl.image_id > 0 ? (uint32_t)ctl.image_id : 0,
        .placement_id = ctl.placement_id,
    };
    if (!ctl.valid)
    {
        kitty_fail(&cmd, "error=EINVAL:malformed control data");
        return;
    }

    char action = ctl.action ? ctl.action : 't';
    switch (action)
    {
    case 't':
        cmd.also_display = false;
        handle_transmit(&cmd, payload, payload_len);
        break;
    case 'T':
        cmd.also_display = true;
        handle_transmit(&cmd, payload, payload_len);
        break;
    case 'p':
        handle_placement(&cmd);
        break;
    case 'd':
        handle_delete(&cmd);
        break;
    default:
        kitty_fail(&cmd, "error=EINVAL:unsupported action");
        break;
    }
}
