#!/usr/bin/env python3
"""Enforce `T *name` spacing for pointer declarators.

For a declarator whose base is a known type:
  * at least one space (or a newline) separates the base from the `*`
    run -- `T*name` is rewritten to `T *name`;
  * the `*` run is adjacent to the declared name -- `T * name` and
    `T * * name` are rewritten to `T *name` / `T **name`.

Allowed and left alone:
  * multiple spaces before the `*` (struct-field alignment);
  * a `*` at end of line (the return-type-on-its-own-line rule);
  * spacing around pointer qualifiers (`char *const *p`);
  * function-pointer declarators (`bool (*fn)(int)`), whose `*` is not
    adjacent to a type;
  * expressions such as `width * height`, since the token before the
    `*` is not a type name.

Comments, strings, and preprocessor logical lines are skipped by the
shared tokenizer.
"""
import glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from format_signatures import tokenize, macro_lines, PRIMITIVE, STD_TYPEDEFS

QUAL = {'const', 'volatile', 'restrict'}
STORAGE = {'static', 'extern', 'inline', '_Noreturn', 'auto', 'register', 'typedef'}
BASE = set(PRIMITIVE) | set(STD_TYPEDEFS) | QUAL | STORAGE | {'struct', 'union', 'enum'}
THIRD_PARTY = ('Ghostty', 'drmMode', 'libinput_', 'xkb_', 'gbm_', 'FT_', 'hb_', 'PNG', 'cairo_')
IDENT = re.compile(r'[A-Za-z_]\w*')


def line_start(s, p):
    return s.rfind('\n', 0, p) + 1


def is_type_token(t):
    return t in BASE or t.endswith('_t') or t.startswith(THIRD_PARTY)


def is_type_at(toks, i):
    if i < 0:
        return False
    t = toks[i][1]
    if is_type_token(t):
        return True
    return (i > 0 and toks[i - 1][1] in ('struct', 'union', 'enum')
            and IDENT.fullmatch(t) is not None)


def edits_for(s):
    toks = tokenize(s)
    macro = macro_lines(s)
    edits = []
    for i, (p, t) in enumerate(toks):
        if t != '*':
            continue
        if line_start(s, p) in macro:
            continue
        prev = toks[i - 1][1] if i else ''
        if prev == '*' or prev in QUAL:
            continue
        if not is_type_at(toks, i - 1):
            continue

        j = i
        while j + 1 < len(toks) and toks[j + 1][1] == '*':
            j += 1
        nxt = toks[j + 1][1] if j + 1 < len(toks) else ''
        if not (IDENT.fullmatch(nxt) or nxt in QUAL or nxt in (')', ']')):
            continue

        before = s[toks[i - 1][0] + len(prev):p]
        if before == '':
            edits.append((p, p, ' '))
        for k in range(i, j):
            st, en = toks[k][0] + 1, toks[k + 1][0]
            if s[st:en] and '\n' not in s[st:en]:
                edits.append((st, en, ''))
        if j + 1 < len(toks) and IDENT.fullmatch(nxt):
            st, en = toks[j][0] + 1, toks[j + 1][0]
            if s[st:en] and '\n' not in s[st:en]:
                edits.append((st, en, ''))
    return edits


def rewrite(s):
    edits = sorted(set(edits_for(s)), reverse=True)
    for st, en, rep in edits:
        s = s[:st] + rep + s[en:]
    return s, len(edits)


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
