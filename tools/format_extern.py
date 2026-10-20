#!/usr/bin/env python3
"""Require an explicit `extern` on file-scope function prototypes.

A function declaration that has no body -- a prototype -- and that is
neither `static` nor a `typedef` must say `extern` explicitly:

    extern size_t ghostcon_mouse_encode(const ghostcon_screen_t *screen,
                                         ghostcon_mouse_button_t button,
                                         ...);

C11 6.7.2p4 gives such a declaration external linkage "exactly as if"
it were declared with the storage-class specifier `extern`, so this
changes no semantics and no generated code. It only makes the linkage
visible where the declaration is written -- which is how header-declared
*variables* must already be spelled:

    extern const ghostcon_style_t GHOSTCON_STYLE_DEFAULT;

Left alone:
  * definitions -- a body follows the parameter list;
  * `static` prototypes -- internal linkage, where `extern` would
    contradict it;
  * function-pointer typedefs (`typedef void (*fn)(int)`);
  * declarations whose specifiers or parameters span a preprocessor
    line, which cannot be rewritten safely.

Wrapped parameter lists keep their alignment: the continuation lines
are aligned under the open paren, which moves right by the length of
the inserted keyword, so those lines are shifted to match.
"""
import glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from format_signatures import (tokenize, macro_lines, line_start, line_end,
                               TYPE_TOKENS, IDENT, ATTRS, analyze_protos)

EXTERN = 'extern '


def line_offsets(s):
    """Char offset of the start of every line."""
    offs = [0]
    for i, ch in enumerate(s):
        if ch == '\n':
            offs.append(i + 1)
    return offs


def find_prototypes(s, toks, ms):
    """Yield (spec_pos, open_pos, close_pos) char offsets for file-scope
    function prototypes that carry no storage-class specifier of their own."""
    is_macro = [line_start(s, p) in ms for p, _ in toks]
    out = []
    for tstart, name_i, open_i, close_i in analyze_protos(s, toks, ms):
        spec = [toks[k][1] for k in range(tstart, name_i)]
        if not spec:
            continue  # no return type: not a declaration we can rewrite
        if 'typedef' in spec or 'static' in spec or 'extern' in spec:
            continue
        if '(' in spec:
            continue  # function-pointer return type
        if any(is_macro[k] for k in range(tstart, close_i + 1)):
            continue
        out.append((toks[tstart][0], toks[open_i][0], toks[close_i][0]))
    return out


def rewrite(s):
    toks = tokenize(s)
    ms = macro_lines(s)
    protos = find_prototypes(s, toks, ms)
    if not protos:
        return s, 0
    edits = []
    offs = line_offsets(s)
    for spec_pos, open_pos, close_pos in protos:
        spec_idx = offs.index(line_start(s, spec_pos))
        open_idx = offs.index(line_start(s, open_pos))
        close_idx = offs.index(line_start(s, close_pos))
        # Two wrapping styles are in use. When the parameter list starts on
        # the same line as the open paren, the continuation lines are
        # aligned under that paren and must move right with it. When the
        # open paren ends its line, the parameters start on the next line
        # with a fixed hanging indent, which the keyword does not affect.
        open_line_end = line_end(s, offs[open_idx])
        paren_aligned = s[open_pos + 1:open_line_end].strip() != ''
        if paren_aligned:
            for idx in range(open_idx + 1, close_idx + 1):
                ln = offs[idx]
                le = line_end(s, ln)
                if not s[ln:le].strip():
                    continue
                edits.append((ln, ln, ' ' * len(EXTERN)))
        edits.append((spec_pos, spec_pos, EXTERN))
    edits.sort()
    clean = []
    last = -1
    for st, en, rep in edits:
        if st < last:
            continue
        clean.append((st, en, rep))
        last = en
    out = []
    cur = 0
    for st, en, rep in clean:
        out.append(s[cur:st])
        out.append(rep)
        cur = en
    out.append(s[cur:])
    return ''.join(out), len(protos)


def main():
    apply = '--apply' in sys.argv
    check = '--check' in sys.argv
    files = sorted(set(glob.glob('src/**/*.[ch]', recursive=True)
                       + glob.glob('include/**/*.[ch]', recursive=True)
                       + glob.glob('tools/**/*.[ch]', recursive=True)
                       + glob.glob('tests/**/*.[ch]', recursive=True)))
    total = touched = 0
    for f in files:
        src = open(f, encoding='utf-8').read()
        new, n = rewrite(src)
        if n:
            total += n
            touched += 1
            if apply:
                open(f, 'w', encoding='utf-8').write(new)
    if check and total:
        print(f"FAIL: {total} edits needed in {touched} files")
        sys.exit(1)
    print(f"{'APPLIED' if apply else 'DRY RUN'}: {total} edits in {touched} files")


if __name__ == '__main__':
    main()
