#!/usr/bin/env bash
# Fetches the Unicode Character Database's UnicodeData.txt into
# third_party/ -- the input tools/gen_symbol_table.py reads to
# (re)generate src/term/cell_symbol_table.gen.c. Not vendored in git
# (see .gitignore) since it's a large, easily re-fetched upstream
# file; run this before rerunning gen_symbol_table.py.
#
# The version is PINNED, not "latest".
#
# The generated table is a snapshot of one UCD version, and
# tests/test_symbol_table.c cross-checks it against the UCD. If the two
# came from different versions the cross-check would compare the table
# against a *newer* UCD and fail on every codepoint Unicode has added
# since -- a failure about the UCD's release cadence, not about the
# table.
#
# (That is not hypothetical: Unicode 16.0 gave U+20C2..U+20CF to new
# currency signs, so a table built from 15.x and a "latest" UCD disagree
# about 143 codepoints.)
#
# To move to a new Unicode version:
#
#   1. bump UCD_VERSION below,
#   2. tools/fetch_ucd.sh
#   3. python3 tools/gen_symbol_table.py third_party/UnicodeData.txt \
#          src/term/cell_symbol_table.gen.c
#   4. commit the regenerated table
#
# Step 4 is what the version marker in the generated file's header is for:
# it records the UCD the table matches, so a reader (and the CI) can tell
# which input it was built from.
set -euo pipefail

# The Unicode version the committed table was generated from.
UCD_VERSION="${UCD_VERSION:-17.0.0}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/third_party/UnicodeData.txt"
VERSION_FILE="$ROOT/third_party/UCD_VERSION"
URL="https://www.unicode.org/Public/$UCD_VERSION/ucd/UnicodeData.txt"

mkdir -p "$ROOT/third_party"
echo "Fetching $URL -> $OUT" >&2
curl -sf --max-time 30 "$URL" -o "$OUT"
echo "$UCD_VERSION" > "$VERSION_FILE"
wc -l "$OUT"
