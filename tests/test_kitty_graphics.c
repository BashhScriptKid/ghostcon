/* test_kitty_graphics
 *
 * Unit-tests the Kitty graphics protocol path (term/kitty_graphics.c) through its
 * public entry point, ghostcon_kitty_graphics_handle().
 *
 * The module had no test file at all, which is not great for a file that decides
 * which paths the terminal opens: the shared-memory, temporary-file and
 * plain-file media are the ones with a security dimension (the temp-file medium
 * deletes the file it reads, so it must only ever delete a file that was created
 * for the handoff), and the chunked-transfer path carries placement state across
 * chunks.
 *
 * The tests here therefore cover three things:
 *
 * - the transmission/placement/ack protocol itself (what gets stored, what
 *   gets acknowledged, and when the cursor moves);
 * - the medium sandbox: /tmp and /dev/shm only, the t=t filename convention,
 *   and what gets deleted (or not) on each path;
 * - the chunked transfer, including the placement keys that only the first
 *   chunk carries.
 */

#define _DEFAULT_SOURCE /* shm_open()/ftruncate() under -std=c11 without this */

#include "ghostcon/term/base64.h"
#include "ghostcon/term/kitty_graphics.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

/* --- ack capture --- */

typedef struct
{
    char   buf[512];
    size_t len;
} ack_t;

static void ack_sink(void *ud, const uint8_t *data, size_t len)
{
    ack_t *a = ud;
    if (a->len + len < sizeof a->buf)
    {
        memcpy(a->buf + a->len, data, len);
        a->len += len;
        a->buf[a->len] = '\0';
    }
}

/* --- helpers --- */

static void run(ghostcon_kitty_graphics_t *kg, const char *body, ack_t *ack, ghostcon_kitty_cursor_move_t *mv)
{
    ack->len = 0;
    ack->buf[0] = '\0';
    *mv = (ghostcon_kitty_cursor_move_t){0};
    ghostcon_kitty_graphics_handle(kg, body, strlen(body), 3, 4, 10, 20,
                                   ack_sink, ack, mv);
}

/* Runs "G<control>;<base64 payload>". */
static void run_cmd(ghostcon_kitty_graphics_t *kg, const char *control, const uint8_t *payload, size_t payload_len, ack_t *ack, ghostcon_kitty_cursor_move_t *mv)
{
    char b64[8192];
    ghostcon_base64_encode(payload, payload_len, b64, sizeof b64);
    char body[8300];
    snprintf(body, sizeof body, "G%s;%s", control, b64);
    run(kg, body, ack, mv);
}

/* Runs a command whose payload is already base64 (a path, for the file
 * media).
 */
static void run_raw(ghostcon_kitty_graphics_t *kg, const char *control, const char *b64_payload, ack_t *ack, ghostcon_kitty_cursor_move_t *mv)
{
    char body[8300];
    snprintf(body, sizeof body, "G%s;%s", control, b64_payload);
    run(kg, body, ack, mv);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
    {
        fprintf(stderr, "cannot create %s\n", path);
        exit(1);
    }
    if (len && write(fd, data, len) != (ssize_t)len)
    {
        fprintf(stderr, "short write %s\n", path);
        exit(1);
    }
    close(fd);
}

static void write_shm(const char *name, const uint8_t *data, size_t len)
{
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
    {
        fprintf(stderr, "cannot create shm %s\n", name);
        exit(1);
    }
    if (ftruncate(fd, (off_t)len) != 0 || write(fd, data, len) != (ssize_t)len)
    {
        fprintf(stderr, "cannot fill shm %s\n", name);
        exit(1);
    }
    close(fd);
}

/* A 2x2 RGBA PNG (red, green / blue, transparent-ish row). */
static const uint8_t png_2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0a, 0x21, 0x14, 0x00, 0x45, 0xcc, 0x07, 0xf9, 0x66, 0xcb, 0xff,
    0x39, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};

int main(void)
{
    ghostcon_kitty_graphics_t kg;
    ack_t ack;
    ghostcon_kitty_cursor_move_t mv;

    uint8_t raw12[12];
    for (int i = 0; i < 12; i++)
        raw12[i] = (uint8_t)(i * 20);

    /* --- raw RGB transmission --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(strcmp(ack.buf, "\x1b_Gi=1;OK\x1b\\") == 0,
          "raw RGB transmit: the terminal acknowledges with OK");
    const ghostcon_kitty_image_t *img = ghostcon_kitty_graphics_find_image(&kg, 1);
    CHECK(img != NULL, "raw RGB transmit: the image is stored");
    CHECK(img && img->width == 2 && img->height == 2 && img->bpp == 3,
          "raw RGB transmit: the declared 2x2 RGB dimensions are kept");
    CHECK(img && img->pixel_len == 12, "raw RGB transmit: 2x2x3 bytes are stored");
    CHECK(img && img->pixels && img->pixels[0] == 0 && img->pixels[1] == 20,
          "raw RGB transmit: the decoded pixels are the transmitted ones");
    CHECK(img && img->generation == 1, "raw RGB transmit: the generation counter advanced");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- the payload size has to match the declared size --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=1", raw12, sizeof raw12 - 1, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:size mismatch") != NULL,
          "short payload: the terminal rejects the size mismatch");
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 1) == NULL,
          "short payload: no image is left behind");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- missing / out-of-range dimensions --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=0,v=2,i=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:missing or out-of-range s=/v=") != NULL,
          "missing s=: the terminal rejects the transmission");
    run_cmd(&kg, "a=t,f=24,s=99999,v=2,i=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:missing or out-of-range s=/v=") != NULL,
          "out-of-range s=: the terminal rejects the transmission");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- unsupported format and medium --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=99,s=2,v=2,i=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EBADF:unsupported format") != NULL,
          "unknown f=: the terminal rejects the format");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=1,t=x", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EBADF:unsupported transmission medium") != NULL,
          "unknown t=: the terminal rejects the medium");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- acks --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=0", raw12, sizeof raw12, &ack, &mv);
    CHECK(ack.len == 0,
          "i=0: the terminal stays silent (no id to attach a response to)");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=2,q=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(ack.len == 0, "q=1: a successful transmission is not acknowledged");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=2,q=1,f=99", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EBADF:unsupported format") != NULL,
          "q=1: an error is still reported");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=2,q=2,f=99", raw12, sizeof raw12, &ack, &mv);
    CHECK(ack.len == 0, "q=2: errors are suppressed too");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=3,p=7", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "i=3,p=7") != NULL,
          "p=: the acknowledgement names the placement");
    run_cmd(&kg, "a=t,f=24,s=2,v=2", raw12, sizeof raw12, &ack, &mv);
    CHECK(ack.len == 0, "no i=: the terminal stays silent");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=4,garbage", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:malformed control data") != NULL,
          "a control key without '=': the terminal rejects the command");
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=4,I=9,o=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(strcmp(ack.buf, "\x1b_Gi=4;OK\x1b\\") == 0,
          "unknown control keys are ignored, not rejected");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- display and placement --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=10", raw12, sizeof raw12, &ack, &mv);
    CHECK(kg.placements[0].in_use == false,
          "a=t: a plain transmission creates no placement");
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=11,z=-1", raw12, sizeof raw12, &ack, &mv);
    CHECK(kg.placements[0].in_use && kg.placements[0].image_id == 11,
          "a=T: the transmission is displayed as a placement");
    CHECK(kg.placements[0].anchor_col == 3 && kg.placements[0].anchor_row == 4,
          "a=T: the placement is anchored at the cursor");
    CHECK(kg.placements[0].z == -1,
          "a=T: a negative z= is kept (behind the text), not coerced to 0");
    CHECK(mv.moved && mv.rows == 1 && mv.col == 5,
          "a=T: the cursor moves past the placement");
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=12,C=1", raw12, sizeof raw12, &ack, &mv);
    CHECK(!mv.moved, "C=1: the cursor does not move");
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=13,c=2,r=3", raw12, sizeof raw12, &ack, &mv);
    CHECK(mv.moved && mv.rows == 3 && mv.col == 6,
          "c=/r=: the cursor moves by the cell-scaled size");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- the placement action on an already transmitted image --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=2,v=2,i=20", raw12, sizeof raw12, &ack, &mv);
    run_cmd(&kg, "a=p,i=20", raw12, sizeof raw12, &ack, &mv);
    CHECK(strcmp(ack.buf, "\x1b_Gi=20;OK\x1b\\") == 0,
          "a=p: the placement is acknowledged");
    CHECK(kg.placements[0].in_use && kg.placements[0].image_id == 20,
          "a=p: the placement references the transmitted image");
    run_cmd(&kg, "a=p,i=99", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=ENOENT:no image with that id") != NULL,
          "a=p on an unknown image: the terminal reports ENOENT");
    run_cmd(&kg, "a=p,i=20,x=0,y=0,w=99,h=99", raw12, sizeof raw12, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:crop rect out of bounds") != NULL,
          "a=p with a crop outside the image: the terminal rejects it");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- chunked transfer ---
     *
     * Only the first chunk carries the placement keys (i=, a=, z=, the crop, the
     * cell scale, C=, p=)
     *
     * the final chunk may omit all of them. The placement the final chunk creates
     * must still use the first chunk's values.
     */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=30,m=1,z=-2,c=1,r=2,p=9", raw12, 6, &ack, &mv);
    CHECK(ack.len == 0, "an intermediate chunk is not acknowledged");
    run_cmd(&kg, "m=0", raw12 + 6, 6, &ack, &mv);
    CHECK(strcmp(ack.buf, "\x1b_Gi=30;OK\x1b\\") == 0,
          "the final chunk of a transfer is acknowledged");
    img = ghostcon_kitty_graphics_find_image(&kg, 30);
    CHECK(img != NULL && img->pixel_len == 12,
          "a chunked transfer stores the whole image");
    CHECK(kg.placements[0].in_use && kg.placements[0].z == -2,
          "a chunked transfer keeps the first chunk's z=");
    CHECK(kg.placements[0].cell_cols == 1 && kg.placements[0].cell_rows == 2,
          "a chunked transfer keeps the first chunk's cell scale");
    CHECK(kg.placements[0].placement_id == 9,
          "a chunked transfer keeps the first chunk's p=");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- a chunk that overshoots the declared size --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=1,v=1,i=31,m=1", raw12, 2, &ack, &mv);
    run_cmd(&kg, "m=1", raw12, 4, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:payload exceeds declared size") != NULL,
          "a chunk past the declared size is rejected");
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 31) == NULL,
          "a rejected chunked transfer leaves no image behind");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- a chunked transfer whose chunks do not add up to the declared size --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=t,f=24,s=1,v=1,i=32,m=1", raw12, 2, &ack, &mv);
    run_cmd(&kg, "m=0", raw12, 2, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:size mismatch") != NULL,
          "a chunked transfer that does not add up is rejected");
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 32) == NULL,
          "a size-mismatched transfer leaves no image behind");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- PNG --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=T,f=100,i=40", png_2x2, sizeof png_2x2, &ack, &mv);
    CHECK(strcmp(ack.buf, "\x1b_Gi=40;OK\x1b\\") == 0,
          "a PNG transmission is acknowledged");
    img = ghostcon_kitty_graphics_find_image(&kg, 40);
    CHECK(img != NULL && img->width == 2 && img->height == 2 && img->bpp == 4,
          "a PNG transmission takes its dimensions from the PNG itself");
    CHECK(img && img->pixel_len == 16,
          "a PNG transmission is stored as RGBA");
    run_cmd(&kg, "a=T,f=100,i=41", (const uint8_t *)"not a png", 9, &ack, &mv);
    CHECK(strstr(ack.buf, "error=EINVAL:invalid or unsupported PNG data") != NULL,
          "corrupt PNG data is rejected");
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 41) == NULL,
          "corrupt PNG data leaves no image behind");
    ghostcon_kitty_graphics_deinit(&kg);

    /* --- the shared-memory medium (t=s) --- */
    {
        char name[64];
        snprintf(name, sizeof name, "/ghostcon-test-kitty-%d", (int)getpid());
        char b64[256];
        ghostcon_kitty_graphics_init(&kg);
        write_shm(name, raw12, sizeof raw12);
        ghostcon_base64_encode((const uint8_t *)name, strlen(name), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=50,t=s", b64, &ack, &mv);
        CHECK(strcmp(ack.buf, "\x1b_Gi=50;OK\x1b\\") == 0,
              "t=s: the segment is read and the transmission acknowledged");
        img = ghostcon_kitty_graphics_find_image(&kg, 50);
        CHECK(img != NULL && img->pixel_len == 12,
              "t=s: the segment's contents are the image");
        /* The segment must have been unlinked: a fresh open must fail. */
        int fd = shm_open(name, O_RDONLY, 0);
        CHECK(fd < 0, "t=s: the segment is unlinked after the transfer");
        if (fd >= 0)
            close(fd);

        /* A segment that does not exist is an error, not a crash. */
        char missing[64];
        snprintf(missing, sizeof missing, "/ghostcon-test-kitty-missing-%d", (int)getpid());
        ghostcon_base64_encode((const uint8_t *)missing, strlen(missing), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=51,t=s", b64, &ack, &mv);
        CHECK(strstr(ack.buf, "error=EBADF:unable to read shared memory segment") != NULL,
              "t=s: a missing segment is reported as an error");
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- the temporary-file medium (t=t) --- */
    {
        char path[256];
        snprintf(path, sizeof path, "/tmp/ghostcon-test-kitty-%d-tty-graphics-protocol", (int)getpid());
        char b64[512];
        ghostcon_kitty_graphics_init(&kg);
        write_file(path, raw12, sizeof raw12);
        ghostcon_base64_encode((const uint8_t *)path, strlen(path), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=60,t=t", b64, &ack, &mv);
        CHECK(strcmp(ack.buf, "\x1b_Gi=60;OK\x1b\\") == 0,
              "t=t: the temporary file is read and the transmission acknowledged");
        img = ghostcon_kitty_graphics_find_image(&kg, 60);
        CHECK(img != NULL && img->pixel_len == 12,
              "t=t: the file's contents are the image");
        CHECK(!file_exists(path),
              "t=t: the temporary file is deleted after the transfer");
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- t=t refuses a file that does not follow the naming convention ---
     *
     * Without the convention, "must be under /tmp" alone would let a hostile
     * client ask the terminal to read AND DELETE any unrelated file that
     * happens to live in /tmp.
     */
    {
        char path[256];
        snprintf(path, sizeof path, "/tmp/ghostcon-test-kitty-%d-not-a-handoff", (int)getpid());
        char b64[512];
        ghostcon_kitty_graphics_init(&kg);
        write_file(path, raw12, sizeof raw12);
        ghostcon_base64_encode((const uint8_t *)path, strlen(path), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=61,t=t", b64, &ack, &mv);
        CHECK(strstr(ack.buf, "error=EBADF:unable to read file") != NULL,
              "t=t: a file outside the naming convention is refused");
        CHECK(file_exists(path),
              "t=t: a refused file is NOT deleted");
        unlink(path);
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- the plain-file medium (t=f) --- */
    {
        char path[256];
        snprintf(path, sizeof path, "/tmp/ghostcon-test-kitty-%d-cache.png", (int)getpid());
        char b64[512];
        ghostcon_kitty_graphics_init(&kg);
        write_file(path, raw12, sizeof raw12);
        ghostcon_base64_encode((const uint8_t *)path, strlen(path), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=70,t=f", b64, &ack, &mv);
        CHECK(strcmp(ack.buf, "\x1b_Gi=70;OK\x1b\\") == 0,
              "t=f: the cached file is read and the transmission acknowledged");
        CHECK(file_exists(path),
              "t=f: the cached file is left in place (it is a cache, not a handoff)");
        unlink(path);
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- the medium sandbox: only /tmp and /dev/shm --- */
    {
        char b64[512];
        ghostcon_kitty_graphics_init(&kg);
        /* /proc is refused outright. */
        const char *proc_path = "/proc/self/status";
        ghostcon_base64_encode((const uint8_t *)proc_path, strlen(proc_path), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=80,t=f", b64, &ack, &mv);
        CHECK(strstr(ack.buf, "error=EBADF:unable to read file") != NULL,
              "t=f: /proc is refused");
        /* A path outside /tmp and /dev/shm is refused. */
        const char *outside = "/etc/hostname";
        ghostcon_base64_encode((const uint8_t *)outside, strlen(outside), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=81,t=f", b64, &ack, &mv);
        CHECK(strstr(ack.buf, "error=EBADF:unable to read file") != NULL,
              "t=f: a path outside /tmp and /dev/shm is refused");
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- /dev/shm is inside the sandbox --- */
    {
        char path[256];
        snprintf(path, sizeof path, "/dev/shm/ghostcon-test-kitty-%d-cache.png", (int)getpid());
        char b64[512];
        ghostcon_kitty_graphics_init(&kg);
        write_file(path, raw12, sizeof raw12);
        ghostcon_base64_encode((const uint8_t *)path, strlen(path), b64, sizeof b64);
        run_raw(&kg, "a=T,f=24,s=2,v=2,i=90,t=f", b64, &ack, &mv);
        CHECK(strcmp(ack.buf, "\x1b_Gi=90;OK\x1b\\") == 0,
              "t=f: a file under /dev/shm is accepted");
        unlink(path);
        ghostcon_kitty_graphics_deinit(&kg);
    }

    /* --- deletion --- */
    ghostcon_kitty_graphics_init(&kg);
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=100", raw12, sizeof raw12, &ack, &mv);
    run_cmd(&kg, "a=T,f=24,s=2,v=2,i=101", raw12, sizeof raw12, &ack, &mv);
    run_cmd(&kg, "a=d,d=i,i=100", raw12, sizeof raw12, &ack, &mv);
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 100) == NULL,
          "d=i: the named image is deleted");
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 101) != NULL,
          "d=i: the other image is kept");
    run_cmd(&kg, "a=d,d=a", raw12, sizeof raw12, &ack, &mv);
    CHECK(ghostcon_kitty_graphics_find_image(&kg, 101) == NULL,
          "d=a: every image is deleted");
    CHECK(kg.placements[0].in_use == false,
          "d=a: every placement is deleted");
    ghostcon_kitty_graphics_deinit(&kg);

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
