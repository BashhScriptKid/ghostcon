#!/usr/bin/env python3
"""Reflow C block comments to:

       /* first line
        * continuation
        *
        * next paragraph
        */

Handles both classic multi-line comments and runs of adjacent
single-line "boxed" comments (/* ---- */ / /* Title */ / /* text */),
which collapse into one block. Code/verbatim paragraphs are preserved
(only re-prefixed); license headers, ASCII banners, and inline/trailing
comments are never touched. Width 76 incl. indentation.
"""
import glob, re, sys, textwrap

WIDTH = 76
SKIP_MARKERS = (
    "Copyright", "Permission is hereby granted", "SPDX-License-Identifier",
    "THE SOFTWARE IS PROVIDED", "MERCHANTABILITY",
)

def scan_comments(src):
    i, n = 0, len(src)
    line_start = 0
    out = []
    while i < n:
        c = src[i]
        if c == '\n':
            line_start = i + 1; i += 1; continue
        if c == '/' and i + 1 < n and src[i+1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '"' or c == "'":
            q = c; i += 1
            while i < n:
                if src[i] == '\\': i += 2; continue
                if src[i] == q: i += 1; break
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i+1] == '*':
            start = i
            end = src.find('*/', i + 2)
            if end < 0: return out
            end += 2
            out.append((start, end, src[line_start:start].strip() == ''))
            i = end
            continue
        i += 1
    return out

def strip_marker(line):
    s = line.strip()
    if s.startswith('*'):
        s = s[1:]
        if s.startswith(' '): s = s[1:]
    return s

def is_separator(s):
    return bool(re.fullmatch(r'[-=_*/\\| ]{4,}', s))

def is_verbatim_line(s):
    if s == '': return False
    if '\t' in s: return True
    if len(s) - len(s.lstrip(' ')) >= 2: return True
    if s[0] in '|+\\': return True
    if s.startswith('#') or s.startswith('//'): return True
    if re.search(r'\s{2,}\S', s): return True
    if re.match(r'^(return|if|for|while|switch)\s*\(', s): return True
    if re.match(r'^(case\s+.+:|break\s*;|continue\s*;)$', s): return True
    if '\x1b' in s: return True
    return False

def paragraphs_of(lines):
    paras, cur = [], []
    for l in lines:
        if l == '':
            if cur: paras.append(cur); cur = []
        else:
            cur.append(l)
    if cur: paras.append(cur)
    return paras

def render(indent, lines, allow_single=True):
    while lines and (lines[0] == '' or is_separator(lines[0])): lines.pop(0)
    while lines and (lines[-1] == '' or is_separator(lines[-1])): lines.pop()
    paras = paragraphs_of(lines)
    if not paras:
        return None
    if allow_single and len(paras) == 1 and len(paras[0]) == 1:
        txt = paras[0][0]
        if (len(indent) + 6 + len(txt) <= WIDTH and not is_verbatim_line(txt)
                and not is_separator(txt)):
            return indent + '/* ' + txt + ' */'
    out = []
    first_para = True
    for pi, para in enumerate(paras):
        if pi > 0:
            out.append(indent + ' *')
        if any(is_verbatim_line(s) for s in para):
            for k, s in enumerate(para):
                if first_para and k == 0:
                    out.append(indent + '/*' + (' ' + s if s else ''))
                else:
                    out.append(indent + ' *' + (' ' + s if s else ''))
            first_para = False
            continue
        text = para[0]
        for nxt in para[1:]:
            text += nxt if text.endswith('-') else ' ' + nxt
        m = re.match(r'^([-*]|\d+[.)])\s+(.*)$', text)
        avail = WIDTH - len(indent) - 3
        if m:
            marker, rest = m.group(1), m.group(2)
            prefix = marker + ' '
            wrapped = textwrap.wrap(rest, width=avail - len(prefix),
                                    break_long_words=False, break_on_hyphens=False)
            out.append(indent + ' * ' + prefix + (wrapped[0] if wrapped else ''))
            for w in wrapped[1:]:
                out.append(indent + ' * ' + ' ' * len(prefix) + w)
            first_para = False
            continue
        wrapped = textwrap.wrap(text, width=avail,
                                break_long_words=False, break_on_hyphens=False)
        for k, w in enumerate(wrapped):
            out.append(indent + ('/* ' if first_para and k == 0 else ' * ') + w)
        first_para = False
    out.append(indent + ' */')
    return '\n'.join(out)

def process(src):
    spans = scan_comments(src)
    edits = []  # (start, end, replacement)
    skipped = set()

    # --- runs of adjacent single-line boxed comments ---
    i = 0
    while i < len(spans):
        start, end, standalone = spans[i]
        if not standalone or '\n' in src[start:end]:
            i += 1; continue
        line_start = src.rfind('\n', 0, start) + 1
        indent = src[line_start:start]
        if indent.strip():
            i += 1; continue
        group = [(start, end)]
        j = i + 1
        while j < len(spans):
            nstart, nend, nstand = spans[j]
            if not nstand or '\n' in src[nstart:nend]:
                break
            nls = src.rfind('\n', 0, nstart) + 1
            nindent = src[nls:nstart]
            between = src[group[-1][1]:nstart]
            if nindent != indent or not re.fullmatch(r'[ \t]*\n[ \t]*', between):
                break
            group.append((nstart, nend)); j += 1
        if len(group) < 2:
            i += 1; continue
        inner = []
        for (s0, e0) in group:
            body = src[s0:e0]
            txt = body[2:-2].strip()
            if not txt or is_separator(txt):
                inner.append('')
            else:
                inner.append(txt)
        # Leave pure-separator groups (floating rulers) untouched
        if not any(inner):
            i = j; continue
        if any(m in src[group[0][0]:group[-1][1]] for m in SKIP_MARKERS):
            i = j; continue
        new = render(indent, inner)
        if new is not None:
            edits.append((group[0][0], group[-1][1], new[len(indent):]))
        i = j

    # --- multi-line convention comments ---
    for start, end, standalone in spans:
        body = src[start:end]
        if '\n' not in body or not standalone:
            continue
        if any(m in body for m in SKIP_MARKERS):
            continue
        if '\\\n' in body or '\\\r\n' in body:
            continue  # inside a #define continuation -- reflowing would
                      # drop the backslashes and end the macro early
        raw_lines = body.split('\n')
        # Already in the target style? Leave it exactly as written (this
        # also preserves hand-formatted examples like config.h's field docs).
        tail = raw_lines[1:]
        if tail and all((not l.strip()) or l.lstrip().startswith('*') for l in tail):
            continue
        line_start = src.rfind('\n', 0, start) + 1
        indent = src[line_start:start]
        if indent.strip():
            continue
        lines = body.split('\n')
        first = lines[0][lines[0].find('/*') + 2:]
        last = lines[-1][:lines[-1].rfind('*/')]
        inner = [strip_marker(first)] + [strip_marker(l) for l in lines[1:-1]] + [strip_marker(last)]
        if any(is_separator(l) for l in inner):
            continue
        new = render(indent, inner, allow_single=False)
        if new is not None and new.strip() != body.strip():
            edits.append((start, end, new[len(indent):]))

    # Apply non-overlapping edits (runs first; skip overlapping multi-line)
    edits.sort()
    merged, last_end = [], -1
    for s, e, rep in edits:
        if s < last_end:
            continue
        merged.append((s, e, rep)); last_end = e
    out, last = [], 0
    for s, e, rep in merged:
        out.append(src[last:s]); out.append(rep); last = e
    out.append(src[last:])
    return ''.join(out), len(merged)

def main():
    apply = '--apply' in sys.argv
    check = '--check' in sys.argv
    files = sorted(set(
        glob.glob('src/**/*.[ch]', recursive=True) +
        glob.glob('include/**/*.[ch]', recursive=True) +
        glob.glob('tools/**/*.[ch]', recursive=True) +
        glob.glob('tests/**/*.[ch]', recursive=True)))
    total = touched = 0
    for f in files:
        src = open(f, encoding='utf-8').read()
        new, n = process(src)
        if n:
            touched += 1; total += n
            if apply:
                open(f, 'w', encoding='utf-8').write(new)
    if check and total:
        print(f"FAIL: {total} comments need reformatting in {touched} files (run with --apply)")
        sys.exit(1)
    print(f"{'APPLIED' if apply else 'DRY RUN'}: {total} comments in {touched} files")

if __name__ == '__main__':
    main()
