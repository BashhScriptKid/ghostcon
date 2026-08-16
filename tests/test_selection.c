/*
 * test_selection -- ghostcon_selection_t state machine, row-range
 * computation, text extraction, and base64 encode/decode. Uses a real
 * ghostcon_term_t (fed plain text through the normal VT parser, same
 * pattern as tests/test_osc.c) so extract_text exercises real cell
 * content rather than hand-poked arrays.
 */

#include "ghostcon/term/term.h"
#include "ghostcon/term/base64.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

int
main(void)
{
    /* --- selection state machine --- */
    ghostcon_selection_t sel;
    memset(&sel, 0, sizeof(sel));

    ghostcon_selection_start(&sel, 2, 0, GC_SEL_CHAR, 80);
    CHECK(sel.active && sel.pending, "start() marks active and pending");
    CHECK(sel.x1 == 2 && sel.y1 == 0 && sel.x2 == 2 && sel.y2 == 0, "start() sets both endpoints to the click");

    ghostcon_selection_update(&sel, 5, 0);
    CHECK(sel.x2 == 5 && sel.y2 == 0, "update() moves the second endpoint");

    ghostcon_selection_finish(&sel);
    CHECK(!sel.pending && sel.active, "finish() clears pending but stays active");

    ghostcon_selection_update(&sel, 9, 0);
    CHECK(sel.x2 == 5, "update() after finish() is a no-op (not pending)");

    ghostcon_selection_clear(&sel);
    CHECK(!sel.active && !sel.pending, "clear() deactivates");

    /* --- row_range: single-row selection --- */
    ghostcon_selection_start(&sel, 5, 3, GC_SEL_CHAR, 80);
    ghostcon_selection_update(&sel, 2, 3); /* dragged backwards */
    ghostcon_selection_finish(&sel);

    int16_t xs, xe;
    CHECK(ghostcon_selection_row_range(&sel, 3, 80, &xs, &xe), "row_range finds the selected row");
    CHECK(xs == 2 && xe == 5, "row_range sorts a backwards single-row drag");
    CHECK(!ghostcon_selection_row_range(&sel, 4, 80, &xs, &xe), "row_range rejects a row outside the selection");

    /* This is the exact bug ghostcon_selection_contains() used to have
       before row_range() was factored out: a single-row selection's
       right bound must be respected, not just its left bound. */
    CHECK(ghostcon_selection_contains(&sel, 2, 3), "contains() includes the left edge");
    CHECK(ghostcon_selection_contains(&sel, 5, 3), "contains() includes the right edge");
    CHECK(!ghostcon_selection_contains(&sel, 6, 3), "contains() excludes past the right edge");
    CHECK(!ghostcon_selection_contains(&sel, 1, 3), "contains() excludes before the left edge");

    /* --- row_range: multi-row selection, drag direction reversed --- */
    ghostcon_selection_clear(&sel);
    ghostcon_selection_start(&sel, 10, 2, GC_SEL_CHAR, 80); /* start lower-right */
    ghostcon_selection_update(&sel, 3, 0);                  /* end upper-left */
    ghostcon_selection_finish(&sel);

    CHECK(ghostcon_selection_row_range(&sel, 0, 80, &xs, &xe) && xs == 3 && xe == 79,
          "multi-row: top row starts at the upper endpoint's x, runs to the row edge");
    CHECK(ghostcon_selection_row_range(&sel, 1, 80, &xs, &xe) && xs == 0 && xe == 79,
          "multi-row: middle row spans the full width");
    CHECK(ghostcon_selection_row_range(&sel, 2, 80, &xs, &xe) && xs == 0 && xe == 10,
          "multi-row: bottom row starts at the row edge, ends at the lower endpoint's x");

    /* --- extract_text, via a real term --- */
    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, 20, 5, 0)) {
        fprintf(stderr, "FAIL: term_init\n");
        return 1;
    }

    const char *text = "hello world\r\nfoo bar";
    ghostcon_term_feed(&term, (const uint8_t *)text, strlen(text));

    char buf[256];
    /* Select "hello world" on row 0 only. */
    ghostcon_selection_clear(&term.screen.selection);
    ghostcon_selection_start(&term.screen.selection, 0, 0, GC_SEL_CHAR, term.screen.cols);
    ghostcon_selection_update(&term.screen.selection, 10, 0);
    ghostcon_selection_finish(&term.screen.selection);

    size_t n = ghostcon_selection_extract_text(&term.screen, buf, sizeof(buf));
    CHECK(n == strlen("hello world") && strcmp(buf, "hello world") == 0,
          "extract_text: single row, trailing padding trimmed");

    /* Select across both rows -- newline between them since row 0 is
       not a wrap continuation of row 1 (it's a real \r\n). */
    ghostcon_selection_clear(&term.screen.selection);
    ghostcon_selection_start(&term.screen.selection, 0, 0, GC_SEL_CHAR, term.screen.cols);
    ghostcon_selection_update(&term.screen.selection, 6, 1);
    ghostcon_selection_finish(&term.screen.selection);

    n = ghostcon_selection_extract_text(&term.screen, buf, sizeof(buf));
    CHECK(n > 0 && strcmp(buf, "hello world\nfoo bar") == 0,
          "extract_text: multi-row with a real newline between rows");

    /* Inactive selection returns 0 without touching `buf`. */
    ghostcon_selection_clear(&term.screen.selection);
    n = ghostcon_selection_extract_text(&term.screen, buf, sizeof(buf));
    CHECK(n == 0, "extract_text: inactive selection returns 0");

    /* Truncation: a tiny out_len must not overflow. */
    ghostcon_selection_start(&term.screen.selection, 0, 0, GC_SEL_CHAR, term.screen.cols);
    ghostcon_selection_update(&term.screen.selection, 10, 0);
    ghostcon_selection_finish(&term.screen.selection);
    char tiny[4];
    n = ghostcon_selection_extract_text(&term.screen, tiny, sizeof(tiny));
    CHECK(n <= 3, "extract_text: truncates to fit a small buffer");
    CHECK(tiny[n] == '\0', "extract_text: still NUL-terminates when truncated");

    ghostcon_term_deinit(&term);

    /* --- base64 --- */
    uint8_t raw[] = "Hello, ghostcon!";
    char encoded[64];
    size_t elen = ghostcon_base64_encode(raw, strlen((char *)raw), encoded, sizeof(encoded));
    CHECK(elen > 0, "base64_encode succeeds");
    CHECK(strcmp(encoded, "SGVsbG8sIGdob3N0Y29uIQ==") == 0, "base64_encode matches a known vector");

    uint8_t decoded[64] = {0};
    size_t dlen = ghostcon_base64_decode(encoded, decoded, sizeof(decoded));
    CHECK(dlen == strlen((char *)raw), "base64_decode round-trip length matches");
    CHECK(memcmp(decoded, raw, dlen) == 0, "base64_decode round-trip content matches");

    CHECK(ghostcon_base64_encode(raw, 0, encoded, sizeof(encoded)) == 0 || encoded[0] == '\0',
          "base64_encode handles empty input");

    /* Non-multiple-of-3 lengths exercise the padding branches. */
    char enc1[8], enc2[8];
    CHECK(ghostcon_base64_encode((const uint8_t *)"a", 1, enc1, sizeof(enc1)) == 4 &&
          strcmp(enc1, "YQ==") == 0, "base64_encode: 1-byte input pads with ==");
    CHECK(ghostcon_base64_encode((const uint8_t *)"ab", 2, enc2, sizeof(enc2)) == 4 &&
          strcmp(enc2, "YWI=") == 0, "base64_encode: 2-byte input pads with =");

    /* Decode tolerates garbage (newlines, whitespace) rather than erroring. */
    uint8_t d2[16] = {0};
    size_t n2 = ghostcon_base64_decode("YQ==\n  ", d2, sizeof(d2));
    CHECK(n2 == 1 && d2[0] == 'a', "base64_decode skips whitespace/padding gracefully");

    CHECK(ghostcon_base64_encode(raw, strlen((char *)raw), encoded, 2) == 0,
          "base64_encode reports failure when out_len is insufficient");

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
