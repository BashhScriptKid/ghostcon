#!/bin/sh
# run_compare.sh — Phase 0 validation harness.
#
# For every file in the corpus directory, feed the same byte stream into
# both ghostcon and a real Ghostty terminal core (libghostty-vt) and diff
# the resulting screen state.
#
# A corpus file may start with a header line:
#   #gc:cols=80 rows=24
# to set the terminal dimensions (default 80x24).
#
# Usage:
#   run_compare.sh [CORPUS_DIR]
#
# Exit status: 0 if every file matches, 1 if any diff.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CORPUS="${1:-$ROOT/tools/corpus}"

GHOSTTY_DUMP="${GHOSTTY_DUMP:-$HOME/.cache/ghostcon/bin/ghostty_dump}"
GHOSTCON_DUMP="${GHOSTCON_DUMP:-$ROOT/build/src/ghostcon_dump}"

if [ ! -x "$GHOSTTY_DUMP" ] || [ ! -x "$GHOSTCON_DUMP" ]; then
    echo "error: dumpers not built ($GHOSTTY_DUMP / $GHOSTCON_DUMP)" >&2
    exit 2
fi

pass=0
fail=0
failed=""

for f in "$CORPUS"/*; do
    [ -f "$f" ] || continue
    name=$(basename "$f")

    cols=80
    rows=24
    hdr=$(sed -n '1p' "$f")
    case "$hdr" in
        \#gc:*)
            cols=$(echo "$hdr" | sed -n 's/.*cols=\([0-9]*\).*/\1/p')
            rows=$(echo "$hdr" | sed -n 's/.*rows=\([0-9]*\).*/\1/p')
            [ -z "$cols" ] && cols=80
            [ -z "$rows" ] && rows=24
            ;;
    esac

    tmp_a=$(mktemp)
    tmp_b=$(mktemp)
    "$GHOSTTY_DUMP" "$cols" "$rows" "$f" > "$tmp_a"
    "$GHOSTCON_DUMP" "$cols" "$rows" "$f" > "$tmp_b"

    if diff -q "$tmp_a" "$tmp_b" >/dev/null 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed="$failed $name"
        echo "DIFF: $name"
        diff "$tmp_a" "$tmp_b" | head -40
        echo
    fi
    rm -f "$tmp_a" "$tmp_b"
done

echo "---"
echo "compare: $pass passed, $fail failed"
if [ -n "$failed" ]; then
    echo "failed:$failed"
fi
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
