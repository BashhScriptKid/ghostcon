#!/usr/bin/env python3
"""Generates src/term/theme_ghostty_presets.c from Ghostty's bundled
theme files (simple `key = value` format, one file per theme, no
section headers). One-time codegen step -- the output is committed
ordinary source, ghostcon's own build never depends on this input
directory existing."""
import os
import re
import sys

THEMEDIR = sys.argv[1]
OUT = sys.argv[2]

def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

entries = []
skipped = []
for name in sorted(os.listdir(THEMEDIR)):
    path = os.path.join(THEMEDIR, name)
    if not os.path.isfile(path):
        continue
    palette = {}
    bg = fg = cursor = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or '=' not in line:
                continue
            key, _, val = line.partition('=')
            key = key.strip()
            val = val.strip()
            if key == 'palette':
                # val is "N=#hex" (there were two '='s on the line)
                if '=' not in val:
                    continue
                idx, _, hexval = val.partition('=')
                try:
                    idx = int(idx.strip())
                except ValueError:
                    continue
                hexval = hexval.strip()
                if 0 <= idx <= 15 and re.match(r'^#[0-9a-fA-F]{6}$', hexval):
                    palette[idx] = hexval
            elif key == 'background':
                bg = val
            elif key == 'foreground':
                fg = val
            elif key == 'cursor-color':
                cursor = val

    if len(palette) != 16 or bg is None or fg is None:
        skipped.append(name)
        continue
    if cursor is None:
        cursor = fg
    entries.append((name, [palette[i] for i in range(16)], fg, bg, cursor))

with open(OUT, 'w') as f:
    f.write('/* GENERATED FILE -- do not hand-edit.\n')
    f.write('   Produced from Ghostty\'s own bundled theme files (one-time codegen,\n')
    f.write('   see tools/gen_ghostty_themes.py) for real Ghostty theme-name parity --\n')
    f.write('   a `theme = "..."` value copied verbatim out of a real Ghostty config\n')
    f.write('   resolves to the exact same colors here. Case-sensitive exact match,\n')
    f.write('   same as Ghostty\'s own theme names. Looked up as a fallback AFTER\n')
    f.write('   theme.c\'s own small curated preset list. */\n\n')
    f.write('#include "ghostcon/term/theme.h"\n\n')
    f.write('typedef struct {\n')
    f.write('    const char *name;\n')
    f.write('    const char *ansi[16];\n')
    f.write('    const char *fg, *bg, *cursor;\n')
    f.write('} ghostty_theme_preset_t;\n\n')
    f.write(f'const int GHOSTCON_GHOSTTY_PRESET_COUNT = {len(entries)};\n\n')
    f.write('const ghostty_theme_preset_t GHOSTCON_GHOSTTY_PRESETS[] = {\n')
    for name, ansi, fg, bg, cursor in entries:
        ansi_str = ', '.join(f'"{c}"' for c in ansi)
        f.write(f'    {{ "{esc(name)}", {{ {ansi_str} }}, "{fg}", "{bg}", "{cursor}" }},\n')
    f.write('};\n')

print(f"wrote {len(entries)} themes, skipped {len(skipped)}: {skipped}", file=sys.stderr)
