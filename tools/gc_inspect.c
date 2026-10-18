/*
 * gc-inspect — headless terminal-state inspector.
 *
 * Feeds a recorded raw PTY byte stream through ghostcon's real parser
 * and screen model (the same code ghostcon-core runs), then prints the
 * resulting grid cell by cell. No DRM, no EGL, no VT, no display needed.
 *
 * Why this exists (PLAN.md refactor 3.2, and CLAUDE.md's rendering-
 * parity debugging notes): the old dump paths reported only
 * *style*-based background information, so a cell whose background came
 * from the content-tag mechanism (GHOSTCON_CELL_BG_COLOR_PALETTE /
 * _RGB -- what ED/EL/ECH and scroll-produced blank rows use) looked
 * like "default" in every existing tool. This prints the tag AND its
 * payload explicitly, so that blind spot can't recur.
 *
 * Usage:
 *   gc-inspect [options] <file.raw|-> 
 *
 *   -             read the byte stream from stdin
 *   --cols N      grid width  (default 80)
 *   --rows N      grid height (default 24)
 *   --scrollback N max scrollback lines (default 2000)
 *   --dump-cells  accepted for the PLAN.md invocation form; cell-by-cell
 *                 output is already the default (and only) mode
 *   --offset N    view N lines back into scrollback (like Shift+PgUp)
 *
 * Output is one line per cell, greppable:
 *   [row=2 col=5 char='A' cp=0x41 tag=codepoint style=0 wide=narrow link=0]
 *   [row=0 col=0 char=' ' cp=0x0 tag=bg_palette content=PALETTE:4 ...]
 *   [row=1 col=3 char=' ' cp=0x0 tag=bg_rgb content=RGB:#102030 ...]
 */

#define _DEFAULT_SOURCE

#include "ghostcon/term/term.h"
#include "ghostcon/term/cell.h"
#include "ghostcon/term/screen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
tag_name(ghostcon_cell_content_tag_t tag)
{
    switch (tag) {
    case GHOSTCON_CELL_CODEPOINT:          return "codepoint";
    case GHOSTCON_CELL_CODEPOINT_GRAPHEME: return "grapheme";
    case GHOSTCON_CELL_BG_COLOR_PALETTE:   return "bg_palette";
    case GHOSTCON_CELL_BG_COLOR_RGB:       return "bg_rgb";
    }
    return "?";
}

static const char *
wide_name(ghostcon_cell_wide_t wide)
{
    switch (wide) {
    case GHOSTCON_CELL_WIDE_NARROW:      return "narrow";
    case GHOSTCON_CELL_WIDE_WIDE:        return "wide";
    case GHOSTCON_CELL_WIDE_SPACER_TAIL: return "spacer_tail";
    case GHOSTCON_CELL_WIDE_SPACER_HEAD: return "spacer_head";
    }
    return "?";
}

static const char *
semantic_name(ghostcon_cell_semantic_t sem)
{
    switch (sem) {
    case GHOSTCON_CELL_SEMANTIC_OUTPUT: return "output";
    case GHOSTCON_CELL_SEMANTIC_INPUT:  return "input";
    case GHOSTCON_CELL_SEMANTIC_PROMPT: return "prompt";
    }
    return "?";
}

static const char *
cursor_style_name(ghostcon_cursor_style_t style)
{
    switch (style) {
    case GC_CURSOR_DEFAULT:          return "default";
    case GC_CURSOR_BLOCK:            return "block";
    case GC_CURSOR_UNDERLINE:        return "underline";
    case GC_CURSOR_BAR:              return "bar";
    case GC_CURSOR_BLOCK_BLINK:      return "block_blink";
    case GC_CURSOR_UNDERLINE_BLINK:  return "underline_blink";
    case GC_CURSOR_BAR_BLINK:        return "bar_blink";
    }
    return "?";
}

/* Prints `cp` as a UTF-8 sequence inside the single quotes of the
   char='' field, escaping anything non-printable as \xNN so one cell
   always occupies exactly one output line. */
static void
print_char_field(uint32_t cp)
{
    if (cp == 0 || cp < 0x20 || cp == 0x7F) {
        putchar('\'');
        if (cp == 0)
            putchar(' ');
        else
            printf("\\x%02x", cp);
        putchar('\'');
        return;
    }

    char buf[5] = {0};
    if (cp < 0x80) {
        buf[0] = (char)cp;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
    }
    printf("'%s'", buf);
}

static void
dump_cells(ghostcon_screen_t *s)
{
    printf("grid: %ux%u scrollback=%u view_offset=%u alt_screen=%d\n",
           s->cols, s->rows_visible, s->history_count, s->view_offset,
           s->alt_screen_active ? 1 : 0);

    printf("cursor: x=%d y=%d visible=%d style=%s saved=%d\n",
           s->cursor.x, s->cursor.y, s->cursor_visible ? 1 : 0,
           cursor_style_name(s->cursor.cursor_style),
           s->cursor_saved ? 1 : 0);

    printf("modes: origin=%d wrap=%d insert=%d reverse=%d appcursor=%d "
           "sync_output=%d bracketed_paste=%d mouse=%u sgr_mouse=%d "
           "left_right_margin=%d\n",
           s->origin_mode ? 1 : 0, s->auto_wrap ? 1 : 0,
           s->insert_mode ? 1 : 0, s->reverse_video ? 1 : 0,
           s->application_cursor ? 1 : 0, s->synchronized_output ? 1 : 0,
           s->bracketed_paste ? 1 : 0, s->mouse_protocol,
           s->mouse_sgr ? 1 : 0, s->left_right_margin ? 1 : 0);

    if (s->selection.active)
        printf("selection: (%d,%d)-(%d,%d) pending=%d\n",
               s->selection.x1, s->selection.y1,
               s->selection.x2, s->selection.y2,
               s->selection.pending ? 1 : 0);
    else
        printf("selection: none\n");

    printf("cwd: \"%s\"\n", s->cwd);

    for (uint16_t y = 0; y < s->rows_visible; y++) {
        ghostcon_row_t *row = ghostcon_screen_row(s, y);
        if (!row)
            continue;

        for (uint16_t x = 0; x < row->cols; x++) {
            ghostcon_cell_t c = row->cells[x];
            ghostcon_cell_content_tag_t tag = ghostcon_cell_get_tag(c);

            printf("[row=%u col=%u char=", y, x);
            print_char_field(tag == GHOSTCON_CELL_CODEPOINT ||
                             tag == GHOSTCON_CELL_CODEPOINT_GRAPHEME
                                 ? ghostcon_cell_get_codepoint(c)
                                 : 0);
            printf(" cp=0x%x tag=%s", ghostcon_cell_get_codepoint(c), tag_name(tag));

            /* The content tag's own payload -- the field the old dump
               paths omitted, and the whole reason this tool exists. */
            if (tag == GHOSTCON_CELL_BG_COLOR_PALETTE) {
                printf(" content=PALETTE:%u", ghostcon_cell_get_palette_idx(c));
            } else if (tag == GHOSTCON_CELL_BG_COLOR_RGB) {
                ghostcon_rgb_t rgb = ghostcon_cell_get_rgb(c);
                printf(" content=RGB:#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
            }

            printf(" style=%u wide=%s",
                   (unsigned)ghostcon_cell_get_style(c),
                   wide_name(ghostcon_cell_get_wide(c)));

            if (ghostcon_cell_get_hyperlink_id(c))
                printf(" link=%u", (unsigned)ghostcon_cell_get_hyperlink_id(c));
            if (ghostcon_cell_get_protected(c))
                printf(" protected=1");
            if (ghostcon_cell_get_semantic(c) != GHOSTCON_CELL_SEMANTIC_OUTPUT)
                printf(" semantic=%s", semantic_name(ghostcon_cell_get_semantic(c)));

            printf("]\n");
        }
    }
}

static bool
feed_file(ghostcon_term_t *term, const char *path)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gc-inspect: %s: %s\n", path, strerror(errno));
        return false;
    }

    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        ghostcon_term_feed(term, buf, n);

    bool ok = !ferror(f);
    if (ok && f != stdin)
        fclose(f);
    if (!ok)
        fprintf(stderr, "gc-inspect: %s: read error\n", path);
    return ok;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--cols N] [--rows N] [--scrollback N] "
        "[--offset N] [--dump-cells] <file.raw|->\n", argv0);
}

int
main(int argc, char **argv)
{
    uint16_t cols = 80, rows = 24, scrollback = 2000;
    int offset = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-cells") == 0) {
            continue; /* default (and only) output mode */
        } else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) {
            cols = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            rows = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--scrollback") == 0 && i + 1 < argc) {
            scrollback = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            offset = (int)strtol(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage(argv[0]);
            return 2;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (cols == 0 || rows == 0) {
        fprintf(stderr, "gc-inspect: cols/rows must be > 0\n");
        return 2;
    }

    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, cols, rows, scrollback)) {
        fprintf(stderr, "gc-inspect: term_init failed\n");
        return 1;
    }

    /* Terminal-generated replies (DA/DSR/OSC queries) have nowhere to
       go in an offline inspection: leave the output callback unset,
       which the stream treats as "drop". */
    bool ok = feed_file(&term, path);
    if (ok) {
        if (offset > 0)
            ghostcon_screen_scroll_view(&term.screen, offset);
        dump_cells(&term.screen);
    }

    ghostcon_term_deinit(&term);
    return ok ? 0 : 1;
}
