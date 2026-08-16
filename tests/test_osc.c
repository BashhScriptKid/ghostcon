/*
 * test_osc — verifies the manually-parsed OSC tier (term/stream.c's
 * osc_dispatch_manual), since the installed libghostty-vt's OSC C API
 * doesn't expose parameter data for these (see color.h's doc comment).
 * Covers OSC 4/10/11/12/104 (Phase 1 item 8, "implement directly" tier)
 * — both the color-spec parser in isolation and the full set/query
 * round-trip through a real ghostcon_term_t, including query responses
 * written back via the output callback.
 */

#include "ghostcon/term/term.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static void
check_parse(const char *spec, bool expect_ok, uint8_t r, uint8_t g, uint8_t b, const char *label)
{
    GhosttyColorRgb c = {0, 0, 0};
    bool ok = ghostcon_color_parse_spec(spec, &c);
    if (ok != expect_ok) {
        fprintf(stderr, "FAIL: parse %s: expected ok=%d, got %d\n", label, expect_ok, ok);
        failures++;
        return;
    }
    if (expect_ok && (c.r != r || c.g != g || c.b != b)) {
        fprintf(stderr, "FAIL: parse %s: expected (%d,%d,%d), got (%d,%d,%d)\n",
                label, r, g, b, c.r, c.g, c.b);
        failures++;
        return;
    }
    printf("PASS: parse %s\n", label);
}

static char g_output_buf[512];
static size_t g_output_len;

static void
capture_output(void *userdata, const uint8_t *data, size_t len)
{
    (void)userdata;
    size_t n = len;
    if (g_output_len + n > sizeof(g_output_buf))
        n = sizeof(g_output_buf) - g_output_len;
    memcpy(g_output_buf + g_output_len, data, n);
    g_output_len += n;
}

static char g_title_buf[256];
static bool g_title_called;

static void
capture_title(void *userdata, const char *title)
{
    (void)userdata;
    g_title_called = true;
    snprintf(g_title_buf, sizeof(g_title_buf), "%s", title);
}

int
main(void)
{
    /* --- color-spec parser, in isolation --- */
    check_parse("#f00", true, 0xff, 0x00, 0x00, "#RGB short red");
    check_parse("#ff0000", true, 0xff, 0x00, 0x00, "#RRGGBB red");
    check_parse("#ffff0000000000", false, 0, 0, 0, "too many digits (>12) rejected");
    check_parse("#fffff", false, 0, 0, 0, "digit count not divisible by 3 rejected");
    check_parse("#ffffffffffff", true, 0xff, 0xff, 0xff, "#RRRRGGGGBBBB max precision white");
    check_parse("#fff000", true, 0xff, 0xf0, 0x00, "#RRGGBB orange-ish");
    check_parse("rgb:ff/00/00", true, 0xff, 0x00, 0x00, "rgb: 2-digit red");
    check_parse("rgb:ffff/0000/0000", true, 0xff, 0x00, 0x00, "rgb: 4-digit red");
    check_parse("rgb:f/0/0", true, 0xff, 0x00, 0x00, "rgb: 1-digit red (scaled up)");
    check_parse("bogus", false, 0, 0, 0, "unrecognized format rejected");
    check_parse("rgb:ff/00", false, 0, 0, 0, "rgb: missing component rejected");

    char formatted[24];
    ghostcon_color_format_spec((GhosttyColorRgb){0xff, 0x80, 0x00}, formatted, sizeof(formatted));
    CHECK(strcmp(formatted, "rgb:ffff/8080/0000") == 0, "format_spec round-trips 16-bit replication");

    /* --- full OSC round-trip through a real term --- */
    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, 80, 24, 0)) {
        fprintf(stderr, "FAIL: term_init\n");
        return 1;
    }
    ghostcon_term_set_output(&term, capture_output, NULL);

    /* OSC 4: set palette index 1 to red */
    const char *set4 = "\x1b]4;1;#ff0000\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set4, strlen(set4));
    GhosttyColorRgb c1 = ghostcon_palette_resolve(&term.screen.palette, 1);
    CHECK(c1.r == 0xff && c1.g == 0 && c1.b == 0, "OSC 4 sets palette[1] to red");

    /* OSC 4 query: should write back "4;1;rgb:ffff/0000/0000" */
    g_output_len = 0;
    const char *query4 = "\x1b]4;1;?\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)query4, strlen(query4));
    g_output_buf[g_output_len] = '\0';
    CHECK(strcmp(g_output_buf, "\x1b]4;1;rgb:ffff/0000/0000\x1b\\") == 0,
          "OSC 4 query replies with current color");

    /* OSC 10/11/12: set default fg/bg/cursor */
    const char *set10 = "\x1b]10;#00ff00\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set10, strlen(set10));
    CHECK(term.screen.palette.fg_default.g == 0xff, "OSC 10 sets default fg");

    const char *set11 = "\x1b]11;#0000ff\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set11, strlen(set11));
    CHECK(term.screen.palette.bg_default.b == 0xff, "OSC 11 sets default bg");

    const char *set12 = "\x1b]12;#ffff00\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set12, strlen(set12));
    CHECK(term.screen.palette.cursor_color.r == 0xff && term.screen.palette.cursor_color.g == 0xff,
          "OSC 12 sets cursor color");

    /* OSC 11 query */
    g_output_len = 0;
    const char *query11 = "\x1b]11;?\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)query11, strlen(query11));
    g_output_buf[g_output_len] = '\0';
    CHECK(strcmp(g_output_buf, "\x1b]11;rgb:0000/0000/ffff\x1b\\") == 0,
          "OSC 11 query replies with current bg");

    /* OSC 104: reset palette[1] back to the ANSI default (red = 170,0,0) */
    const char *reset104 = "\x1b]104;1\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)reset104, strlen(reset104));
    GhosttyColorRgb c1_reset = ghostcon_palette_resolve(&term.screen.palette, 1);
    CHECK(c1_reset.r == 170 && c1_reset.g == 0 && c1_reset.b == 0,
          "OSC 104 resets palette[1] to ANSI default");

    /* OSC 104 with no args: reset ALL (palette[2] was never touched, so
       just confirm it's still correct after a full reset, and that
       palette[1] -- explicitly set again first -- goes back too). */
    const char *setagain = "\x1b]4;1;#123456\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)setagain, strlen(setagain));
    const char *resetall = "\x1b]104\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)resetall, strlen(resetall));
    GhosttyColorRgb c1_after_full_reset = ghostcon_palette_resolve(&term.screen.palette, 1);
    CHECK(c1_after_full_reset.r == 170 && c1_after_full_reset.g == 0 && c1_after_full_reset.b == 0,
          "OSC 104 (no args) resets every palette entry");

    /* Window title (0/1/2) must still work -- the manual tier must not
       have corrupted st->buf for OSC numbers it doesn't own. */
    const char *title = "\x1b]0;test title\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)title, strlen(title));
    /* No public API currently stores/exposes the title string (that's
       OSC 0/2's own separate item, process-identity repurposing, not
       yet implemented) -- this just needs to not crash or corrupt
       subsequent parsing, which the next check verifies. */
    const char *set4_again = "\x1b]4;2;#abcdef\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set4_again, strlen(set4_again));
    GhosttyColorRgb c2 = ghostcon_palette_resolve(&term.screen.palette, 2);
    CHECK(c2.r == 0xab && c2.g == 0xcd && c2.b == 0xef,
          "OSC 4 still works correctly after a window-title sequence");

    /* OSC 7: report PWD via file://host/path URI, host discarded,
       percent-encoding decoded */
    const char *pwd1 = "\x1b]7;file://myhost/home/user/My%20Docs\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)pwd1, strlen(pwd1));
    CHECK(strcmp(term.screen.cwd, "/home/user/My Docs") == 0,
          "OSC 7 decodes file:// URI, strips host, unescapes %20");

    /* OSC 7: bare path (no file:// wrapper) also accepted */
    const char *pwd2 = "\x1b]7;/tmp\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)pwd2, strlen(pwd2));
    CHECK(strcmp(term.screen.cwd, "/tmp") == 0,
          "OSC 7 accepts a bare path with no file:// wrapper");

    /* OSC 133: FinalTerm semantic prompt markers stamp cells as they're
       printed. A: prompt start, B: input start, C: output start,
       D[;code]: command finished (+ exit code). */
    CHECK(term.screen.semantic_current == GHOSTCON_CELL_SEMANTIC_OUTPUT,
          "semantic state starts as OUTPUT (default)");
    const char *p133_a = "\x1b]133;A\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p133_a, strlen(p133_a));
    const char *prompt_text = "$ ";
    ghostcon_term_feed(&term, (const uint8_t *)prompt_text, strlen(prompt_text));
    ghostcon_cell_t prompt_cell = term.screen.rows[0].cells[0];
    CHECK(ghostcon_cell_get_semantic(prompt_cell) == GHOSTCON_CELL_SEMANTIC_PROMPT,
          "OSC 133;A marks subsequently-printed cells as PROMPT");

    const char *p133_b = "\x1b]133;B\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p133_b, strlen(p133_b));
    const char *input_text = "ls";
    ghostcon_term_feed(&term, (const uint8_t *)input_text, strlen(input_text));
    ghostcon_cell_t input_cell = term.screen.rows[0].cells[2];
    CHECK(ghostcon_cell_get_semantic(input_cell) == GHOSTCON_CELL_SEMANTIC_INPUT,
          "OSC 133;B marks subsequently-printed cells as INPUT");

    const char *p133_c = "\x1b]133;C\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p133_c, strlen(p133_c));
    const char *output_text = "file.txt";
    ghostcon_term_feed(&term, (const uint8_t *)output_text, strlen(output_text));
    ghostcon_cell_t output_cell = term.screen.rows[0].cells[4];
    CHECK(ghostcon_cell_get_semantic(output_cell) == GHOSTCON_CELL_SEMANTIC_OUTPUT,
          "OSC 133;C marks subsequently-printed cells as OUTPUT");

    const char *p133_d = "\x1b]133;D;0\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p133_d, strlen(p133_d));
    CHECK(term.screen.semantic_last_exit_code == 0,
          "OSC 133;D;0 records exit code 0");
    CHECK(term.screen.semantic_current == GHOSTCON_CELL_SEMANTIC_PROMPT,
          "OSC 133;D returns state to PROMPT");

    const char *p133_d_nonzero = "\x1b]133;D;127\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p133_d_nonzero, strlen(p133_d_nonzero));
    CHECK(term.screen.semantic_last_exit_code == 127,
          "OSC 133;D;127 records a nonzero exit code");

    /* OSC 633: VSCode superset -- same A/B/C/D letters, plus a
       Cwd property report that should reuse OSC 7's path decoder. */
    const char *p633_a = "\x1b]633;A\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p633_a, strlen(p633_a));
    CHECK(term.screen.semantic_current == GHOSTCON_CELL_SEMANTIC_PROMPT,
          "OSC 633;A also marks PROMPT (shared letter set with 133)");

    const char *p633_cwd = "\x1b]633;P;Cwd=/var/log\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)p633_cwd, strlen(p633_cwd));
    CHECK(strcmp(term.screen.cwd, "/var/log") == 0,
          "OSC 633;P;Cwd=... updates screen.cwd via the OSC 7 decoder");

    /* OSC 0/2: the term layer hands the raw title string up through a
       callback (ghostcon-core is the one that turns it into argv[0]/
       PR_SET_NAME, which this term-level test has no business doing --
       just verify the plumbing delivers the right string). */
    g_title_called = false;
    g_title_buf[0] = '\0';
    ghostcon_term_set_title(&term, capture_title, NULL);
    const char *title0 = "\x1b]0;my-script.sh\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)title0, strlen(title0));
    CHECK(g_title_called && strcmp(g_title_buf, "my-script.sh") == 0,
          "OSC 0 delivers the title string via ghostcon_term_set_title");

    g_title_called = false;
    const char *title2 = "\x1b]2;another title\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)title2, strlen(title2));
    CHECK(g_title_called && strcmp(g_title_buf, "another title") == 0,
          "OSC 2 delivers the title string via the same callback");

    /* OSC 8: hyperlink start/end round-trip. Start stamps subsequently
       -printed cells with a nonzero hyperlink_id resolving back to the
       URI via the interned hyperlink set; end (empty URI) resets to 0. */
    const char *reset_row = "\r\n"; /* known column 0 -- earlier tests
        left the cursor mid-row, so indexing rows[0].cells[N] directly
        would check the wrong cell */
    ghostcon_term_feed(&term, (const uint8_t *)reset_row, strlen(reset_row));
    int16_t link_row = term.screen.cursor.y;

    const char *link_start = "\x1b]8;;https://example.com\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)link_start, strlen(link_start));
    const char *link_text = "click";
    ghostcon_term_feed(&term, (const uint8_t *)link_text, strlen(link_text));
    ghostcon_cell_t link_cell = term.screen.rows[link_row].cells[0];
    ghostcon_style_id_t link_id = ghostcon_cell_get_hyperlink_id(link_cell);
    CHECK(link_id != 0, "OSC 8 start stamps a nonzero hyperlink_id");
    const char *resolved = ghostcon_hyperlink_set_get(term.screen.hyperlinks, link_id);
    CHECK(resolved && strcmp(resolved, "https://example.com") == 0,
          "hyperlink_id resolves back to the URI via the interned set");

    const char *link_end = "\x1b]8;;\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)link_end, strlen(link_end));
    const char *after_text = "plain";
    ghostcon_term_feed(&term, (const uint8_t *)after_text, strlen(after_text));
    ghostcon_cell_t plain_cell = term.screen.rows[link_row].cells[5];
    CHECK(ghostcon_cell_get_hyperlink_id(plain_cell) == 0,
          "OSC 8 end (empty URI) resets hyperlink_id to 0 for later text");

    /* OSC 9/777: no public storage to inspect (stub tier delivers via a
       callback core/main.c logs to journald) -- just confirm it's
       recognized/handled (returns true from osc_dispatch_manual) rather
       than falling through to the discard path, by checking it doesn't
       disturb subsequent parsing, same regression style as the
       window-title check above. */
    const char *notify9 = "\x1b]9;hello from OSC 9\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)notify9, strlen(notify9));
    const char *notify777 = "\x1b]777;notify;summary;hello from OSC 777\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)notify777, strlen(notify777));
    const char *set4_after_notify = "\x1b]4;3;#112233\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)set4_after_notify, strlen(set4_after_notify));
    GhosttyColorRgb c3 = ghostcon_palette_resolve(&term.screen.palette, 3);
    CHECK(c3.r == 0x11 && c3.g == 0x22 && c3.b == 0x33,
          "parsing continues correctly after OSC 9/777 notifications");

    /* OSC 52: clipboard set + query round-trip, single-instance only. */
    const char *clip_set = "\x1b]52;c;aGVsbG8=\x1b\\"; /* "hello" */
    ghostcon_term_feed(&term, (const uint8_t *)clip_set, strlen(clip_set));
    CHECK(strcmp(term.screen.clipboard, "aGVsbG8=") == 0,
          "OSC 52 set stores the base64 payload verbatim");

    g_output_len = 0;
    const char *clip_query = "\x1b]52;c;?\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)clip_query, strlen(clip_query));
    g_output_buf[g_output_len] = '\0';
    CHECK(strcmp(g_output_buf, "\x1b]52;c;aGVsbG8=\x1b\\") == 0,
          "OSC 52 query replies with the stored payload");

    const char *clip_invalid = "\x1b]52;c;not valid base64!!\x1b\\";
    ghostcon_term_feed(&term, (const uint8_t *)clip_invalid, strlen(clip_invalid));
    CHECK(strcmp(term.screen.clipboard, "aGVsbG8=") == 0,
          "OSC 52 rejects a non-base64 payload, leaving the old value stored");

    ghostcon_term_deinit(&term);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
