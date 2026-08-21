#!/usr/bin/env bash
# Fetches the Unicode Character Database's UnicodeData.txt into
# third_party/ -- the input tools/gen_symbol_table.py reads to
# (re)generate src/term/cell_symbol_table.gen.c. Not vendored in git
# (see .gitignore) since it's a large, easily re-fetched upstream
# file; run this before rerunning gen_symbol_table.py, e.g. when a
# new Unicode version ships.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/third_party/UnicodeData.txt"
URL="https://www.unicode.org/Public/UCD/latest/ucd/UnicodeData.txt"

mkdir -p "$ROOT/third_party"
echo "Fetching $URL -> $OUT" >&2
curl -sf --max-time 30 "$URL" -o "$OUT"
wc -l "$OUT"
