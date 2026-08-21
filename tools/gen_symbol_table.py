#!/usr/bin/env python3
"""Generates src/term/cell_symbol_table.gen.c from the Unicode Character
Database (UnicodeData.txt). One-time codegen step -- the output is
committed ordinary source, ghostcon's own build never depends on the UCD
input file existing. Run tools/fetch_ucd.sh first to fetch the input
(third_party/UnicodeData.txt -- not committed, see .gitignore).

Selects every codepoint whose General Category is Sm, Sc, Sk, or So
(Unicode's "Symbol" categories) -- this mirrors Ghostty's UCD-derived
isSymbol(). The Private Use Area is deliberately NOT included here even
though ghostcon treats it as symbol-like for Nerd Font icon glyphs: PUA
codepoints are General Category Co (Private Use), not Symbol, so real
UCD data has nothing to say about them. That carve-out stays hand-added
in cell.c, layered on top of this generated table, not folded into it.
"""
import sys

IN = sys.argv[1]
OUT = sys.argv[2]

SYMBOL_CATEGORIES = {'Sm', 'Sc', 'Sk', 'So'}

codepoints = []
pending_first = None
with open(IN, 'r', encoding='utf-8') as f:
    for line in f:
        line = line.rstrip('\n')
        if not line:
            continue
        fields = line.split(';')
        cp = int(fields[0], 16)
        name = fields[1]
        category = fields[2]

        if name.endswith(', First>'):
            pending_first = (cp, category)
            continue
        if name.endswith(', Last>'):
            first_cp, first_cat = pending_first
            pending_first = None
            assert first_cat == category, f"range category mismatch at {fields[0]}"
            if category in SYMBOL_CATEGORIES:
                codepoints.append((first_cp, cp))
            continue

        if category in SYMBOL_CATEGORIES:
            codepoints.append((cp, cp))

codepoints.sort()

# Coalesce adjacent/overlapping entries into merged [lo, hi] ranges.
merged = []
for lo, hi in codepoints:
    if merged and lo <= merged[-1][1] + 1:
        merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
    else:
        merged.append((lo, hi))

with open(OUT, 'w') as f:
    f.write('/* GENERATED FILE -- do not hand-edit.\n')
    f.write(' * Produced by tools/gen_symbol_table.py from the Unicode Character\n')
    f.write(' * Database (UnicodeData.txt). Covers General Category Sm, Sc, Sk, So\n')
    f.write(' * ("Symbol" categories) -- mirrors Ghostty\'s UCD-derived isSymbol().\n')
    f.write(' *\n')
    f.write(' * Does NOT include the Private Use Area (Nerd Font icon glyphs) --\n')
    f.write(' * PUA codepoints are category Co, not Symbol. That carve-out is\n')
    f.write(' * hand-added in cell.c on top of this table. See gen_symbol_table.py.\n')
    f.write(' */\n\n')
    f.write('#include "ghostcon/term/cell_symbol_table.h"\n\n')
    f.write(f'const ghostcon_cp_range_t ghostcon_generated_symbol_ranges[] = {{\n')
    for lo, hi in merged:
        f.write(f'    {{ 0x{lo:04X}, 0x{hi:04X} }},\n')
    f.write('};\n\n')
    f.write(f'const size_t ghostcon_generated_symbol_ranges_count = {len(merged)};\n')

print(f'{len(merged)} merged ranges from {len(codepoints)} raw entries -> {OUT}', file=sys.stderr)
