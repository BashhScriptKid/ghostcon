#!/usr/bin/env python3
"""Enforce Allman braces for statement/definition blocks:

    statement
    {
        code
    }

- Functions, if/for/while/switch, else, do, struct/enum/union.
- Same-line single-line bodies are expanded; existing 4-space
  indentation is preserved (new content gets +4).
- Initializers, designated initializers, compound literals, and
  preprocessor macro bodies are never touched.
- Inline code before/after braces is never moved across tokens; only
  whitespace/newlines are rewritten.
"""
import glob, re, sys

TOK = re.compile(r'''
    //[^\n]*
  | /\*.*?\*/
  | "(?:\\.|[^"\\])*"
  | '(?:\\.|[^'\\])*'
  | [A-Za-z_]\w*
  | \.?[0-9][0-9A-Za-z_.]*
  | ->|\+\+|--|<<=|>>=|==|!=|<=|>=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=
  | \S
''', re.S | re.X)

INIT_BEFORE = {'=', '(', ',', 'return', '?', '[', '+', '-', '*', '/',
               '%', '!', '~', '&', '|', '^', '<', '>'}
CONTROL = {'if', 'for', 'while', 'switch'}
BLOCK_KINDS = CONTROL | {'func', 'else', 'do', 'struct'}

def tokenize(s):
    out = []
    for m in TOK.finditer(s):
        t = m.group(0)
        if t.startswith('//') or t.startswith('/*'):
            continue
        out.append((m.start(), t))
    return out

def line_start(s, pos):
    return s.rfind('\n', 0, pos) + 1

def line_end(s, pos):
    e = s.find('\n', pos)
    return len(s) if e < 0 else e

def indent_of(s, pos):
    ls = line_start(s, pos)
    i = ls
    while i < len(s) and s[i] in ' \t':
        i += 1
    return s[ls:i]

def in_macro(s, pos):
    ls = line_start(s, pos)
    # The brace's own line may be the #define line.
    if s[ls:line_end(s, ls)].lstrip().startswith('#'):
        return True
    start = ls
    while start > 0:
        pls = line_start(s, start - 1)
        prev = s[pls:start - 1]
        if prev.rstrip().endswith('\\'):
            start = pls
        else:
            break
    if s[start:ls].lstrip().startswith('#'):
        return True
    return s[ls:line_end(s, ls)].rstrip().endswith('\\')

CODE_ONLY = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
def has_code(text):
    return bool(CODE_ONLY.sub('', text).strip())

def analyze(s):
    toks = tokenize(s)
    n = len(toks)
    # matching parens
    pstack, match_open = [], {}
    for i, (p, t) in enumerate(toks):
        if t == '(':
            pstack.append(i)
        elif t == ')' and pstack:
            match_open[i] = pstack.pop()
    # brace classification + matching close
    bstack, blocks = [], []
    for i, (p, t) in enumerate(toks):
        if t == '{':
            parent = bstack[-1][1] if bstack else None
            prev = toks[i - 1][1] if i else ''
            if parent == 'init':
                kind = 'init'
            elif prev == 'else':
                kind = 'else'
            elif prev == 'do':
                kind = 'do'
            elif prev in ('struct', 'enum', 'union'):
                kind = 'struct'
            elif prev == ')':
                j = match_open.get(i - 1)
                before = toks[j - 1][1] if j else ''
                if before in CONTROL:
                    kind = before
                elif before in INIT_BEFORE:
                    kind = 'init'
                else:
                    kind = 'func'
            elif prev in INIT_BEFORE or prev in ('{', ';', '}', ':'):
                kind = 'init'
            else:
                kind = 'init'
            bstack.append([i, kind, p])
        elif t == '}' and bstack:
            bi, kind, bp = bstack.pop()
            if kind != 'init':
                blocks.append({'kind': kind, 'open': bp, 'close': p,
                               'tok': i, 'open_tok': bi})
    return toks, match_open, blocks

def anchor_pos(toks, match_open, blk):
    kind = blk['kind']
    i = blk['open_tok']
    if kind in CONTROL or kind == 'func':
        j = match_open.get(i - 1)
        return toks[j - 1][0] if j else blk['open']
    if kind == 'else':
        return toks[i - 1][0]
    if kind == 'do':
        return toks[i - 1][0]
    if kind == 'struct':
        return toks[i - 1][0]
    return blk['open']

def rewrite(s):
    toks, match_open, blocks = analyze(s)
    edits = []  # (start, end, replacement)

    for blk in blocks:
        if in_macro(s, blk['open']):
            continue
        indent = indent_of(s, anchor_pos(toks, match_open, blk))
        op, cp = blk['open'], blk['close']

        # 1. opening brace onto its own line if code precedes it
        ls = line_start(s, op)
        if s[ls:op].strip():
            ws = op
            while ws > ls and s[ws - 1] in ' \t':
                ws -= 1
            edits.append((ws, op, '\n' + indent))

        # 2. content starting on the brace line
        le = line_end(s, op)
        tail = s[op + 1:le]
        if has_code(tail):
            ws = op + 1
            while ws < le and s[ws] in ' \t':
                ws += 1
            edits.append((op + 1, ws, '\n' + indent + '    '))

        # 3. closing brace onto its own line when content shares it
        cls = line_start(s, cp)
        between = s[op + 1:cp]
        if '\n' not in between:
            ws = cp
            while ws > op + 1 and s[ws - 1] in ' \t':
                ws -= 1
            edits.append((ws, cp, '\n' + indent))
        elif s[cls:cp].strip():
            ws = cp
            while ws > cls and s[ws - 1] in ' \t':
                ws -= 1
            edits.append((ws, cp, '\n' + indent))

    # 4. else onto its own line when it follows } on the same line
    for i, (p, t) in enumerate(toks):
        if t != 'else' or i == 0 or toks[i - 1][1] != '}':
            continue
        if in_macro(s, p):
            continue
        prev = toks[i - 1][0]
        if line_start(s, prev) != line_start(s, p):
            continue
        indent = indent_of(s, prev)
        ls = line_start(s, p)
        ws = p
        while ws > ls and s[ws - 1] in ' \t':
            ws -= 1
        edits.append((ws, p, '\n' + indent))

    # apply non-overlapping edits back-to-front
    edits.sort()
    clean, last = [], -1
    for st, en, rep in edits:
        if st < last:
            continue
        clean.append((st, en, rep))
        last = en
    out, cur = [], 0
    for st, en, rep in clean:
        out.append(s[cur:st]); out.append(rep); cur = en
    out.append(s[cur:])
    return ''.join(out), len(clean)

def main():
    args = sys.argv[1:]
    apply = '--apply' in args
    check = '--check' in args
    files = sorted(set(
        glob.glob('src/**/*.[ch]', recursive=True) +
        glob.glob('include/**/*.[ch]', recursive=True) +
        glob.glob('tools/**/*.[ch]', recursive=True) +
        glob.glob('tests/**/*.[ch]', recursive=True)))
    total = touched = 0
    for f in files:
        src = open(f, encoding='utf-8').read()
        new, n = rewrite(src)
        if n:
            total += n; touched += 1
            if apply:
                open(f, 'w', encoding='utf-8').write(new)
    if check and total:
        print(f"FAIL: {total} edits needed in {touched} files (run with --apply)")
        sys.exit(1)
    print(f"{'APPLIED' if apply else 'DRY RUN'}: {total} edits in {touched} files")

if __name__ == '__main__':
    main()
