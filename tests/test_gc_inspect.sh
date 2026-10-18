#!/bin/sh
# Regression test for tools/gc-inspect (PLAN.md refactor 3.2).
#
# The key property, learned the hard way (CLAUDE.md's parity-debugging
# notes): a blank cell whose background comes from the content-tag
# mechanism must be reported with its tag AND payload. Older dumps
# reported only style-based backgrounds, so a real erased background
# showed up as "default" in every tool.
#
# Build an 8x3 stream:
#   - fill the screen with palette-4 (blue) background via SGR 44 + ED 2
#   - write "Hi" in default colors (style-based)
#   - fill one cell with an RGB background via SGR 48;2;... + ECH
# Then assert gc-inspect reports both content-tag forms, the chars, and
# the mode header.

set -eu
BIN="${GC_INSPECT_BIN:?GC_INSPECT_BIN must point at tools/gc-inspect}"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# ESC[44m ESC[2J             -> palette background erase everywhere
# ESC[1;1H "Hi"             -> style-based text cells on row 0
# ESC[2;1H SGR 48;2.. ESC[K -> RGB background fill on row 1
printf '\033[44m\033[2J\033[1;1HHi\033[2;1H\033[48;2;16;32;48m\033[K' > "$TMP"

OUT="$("$BIN" --cols 8 --rows 3 "$TMP")"

fail() { echo "FAIL: $*" >&2; exit 1; }

printf '%s\n' "$OUT" | grep -q '^grid: 8x3 ' \
    || fail "missing grid header"
printf '%s\n' "$OUT" | grep -q 'tag=bg_palette content=PALETTE:4' \
    || fail "palette-background erase cells not reported with payload"
printf '%s\n' "$OUT" | grep -q "char='H'" \
    || fail "text cell 'H' missing"
printf '%s\n' "$OUT" | grep -q "char='i'" \
    || fail "text cell 'i' missing"
printf '%s\n' "$OUT" | grep -q 'tag=bg_rgb content=RGB:#102030' \
    || fail "RGB-background erase cell not reported with payload"
printf '%s\n' "$OUT" | grep -q 'row=1 col=0 .*tag=bg_rgb' \
    || fail "expected RGB fill at row 1 col 0 after CUP"

echo "gc-inspect OK"
