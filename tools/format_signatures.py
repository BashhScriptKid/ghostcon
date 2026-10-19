#!/usr/bin/env python3
"""Enforce the signature layout rules for function DEFINITIONS:

  * Portable return type (language keyword or standard/POSIX scalar
    typedef, qualifiers/pointers ignored) -> one line:
        static bool foo(int x)
  * Anything declared by ghostcon or a third-party dependency, or a
    struct/union/enum tag -> split:
        ghostcon_style_id_t
        ghostcon_style_add(...)
  * A pointer return keeps the `*` with the declared name, so it stays
    one line (the `*` is part of the declarator, not the base type):
        ghostcon_style_set_t *ghostcon_style_set_create(...)
  * The parameter list is always collapsed onto the name line, however
    long:
        static void foo(int a, int b)

Prototypes are left alone. Definitions whose return type contains a
comment or a function-pointer declarator are skipped, as are parameter
lists containing line comments, multi-line comments, or preprocessor
lines (anything that can't safely be moved onto one line).
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

STORAGE = {'static','extern','inline','_Noreturn','auto','register','typedef'}
QUALIFIERS = {'const','volatile','restrict','_Atomic'}
ATTRS = {'__attribute__','__declspec','alignas','_Alignas'}
IDENT = re.compile(r'[A-Za-z_]\w*')
PRIMITIVE = {'void','char','short','int','long','float','double',
             'signed','unsigned','_Bool','bool'}
STD_TYPEDEFS = {
    'int8_t','int16_t','int32_t','int64_t','uint8_t','uint16_t','uint32_t','uint64_t',
    'int_least8_t','int_least16_t','int_least32_t','int_least64_t',
    'uint_least8_t','uint_least16_t','uint_least32_t','uint_least64_t',
    'int_fast8_t','int_fast16_t','int_fast32_t','int_fast64_t',
    'uint_fast8_t','uint_fast16_t','uint_fast32_t','uint_fast64_t',
    'intptr_t','uintptr_t','intmax_t','uintmax_t','size_t','ssize_t','ptrdiff_t',
    'wchar_t','wint_t','clock_t','time_t','off_t','pid_t','uid_t','gid_t','mode_t',
    'dev_t','ino_t','nlink_t','blksize_t','blkcnt_t','FILE','va_list',
}
# tokens that can appear in a return-type/declaration range
TYPE_TOKENS = (set(PRIMITIVE) | set(STD_TYPEDEFS) | QUALIFIERS | STORAGE
               | {'struct','union','enum'})

def tokenize(s):
    out=[]
    for m in TOK.finditer(s):
        t=m.group(0)
        if t.startswith('//') or t.startswith('/*'):
            continue
        out.append((m.start(), t))
    return out

def line_start(s,p): return s.rfind('\n',0,p)+1
def line_end(s,p):
    e=s.find('\n',p); return len(s) if e<0 else e

def macro_lines(s):
    """Return set of physical line-start offsets belonging to preprocessor
    logical lines."""
    lines=[]; pos=0
    while pos <= len(s):
        e=s.find('\n',pos); e=len(s) if e<0 else e
        lines.append((pos,e)); 
        if e==len(s): break
        pos=e+1
    mark=set(); i=0
    while i < len(lines):
        st,en=lines[i]
        if s[st:en].lstrip().startswith('#'):
            mark.add(st)
            while s[st:en].rstrip().endswith('\\') and i+1 < len(lines):
                i+=1; st,en=lines[i]; mark.add(st)
        i+=1
    return mark

def indent_of(s,p):
    ls=line_start(s,p); i=ls
    while i<len(s) and s[i] in ' \t': i+=1
    return s[ls:i]

def classify_base(s, toks, tstart, name_i):
    """Return True if the return type is portable (one-line)."""
    i = tstart
    while i < name_i:
        t = toks[i][1]
        if t in STORAGE or t in QUALIFIERS:
            i += 1; continue
        if t in ATTRS:
            # skip attribute including its parenthesized argument
            depth=0; i += 1
            while i < name_i:
                tt=toks[i][1]
                if tt=='(' : depth+=1
                elif tt==')':
                    depth-=1
                    if depth==0: i+=1; break
                i+=1
            continue
        if t in ('struct','union','enum'):
            return False
        return t in PRIMITIVE or t in STD_TYPEDEFS
    return False

def analyze_defs(s, toks, ms):
    """Yield (type_start_tok, name_tok, open_brace_tok) for top-level
    function definitions."""
    excl = set()
    for m in ms: excl.add(m)
    # token index -> on macro line?
    is_macro = []
    for p,_ in toks:
        is_macro.append(line_start(s,p) in ms)
    # brace depth before/after each token
    depth=0; depths=[]
    for p,t in toks:
        depths.append(depth)
        if t=='{': depth+=1
        elif t=='}': depth=max(0,depth-1)
    depth=0; depths_after=[]
    for p,t in toks:
        if t=='{': depth+=1
        elif t=='}': depth=max(0,depth-1)
        depths_after.append(depth)
    # paren matching
    st=[]; mo={}
    for i,(p,t) in enumerate(toks):
        if t=='(': st.append(i)
        elif t==')' and st: mo[i]=st.pop()
    have_brace_target = {}
    for i,(p,t) in enumerate(toks):
        if t=='{':
            have_brace_target.setdefault(depths[i], i)
    defs=[]
    depth=0
    for i,(p,t) in enumerate(toks):
        if is_macro[i]:
            continue
        if t=='{':
            # top-level function definition?
            if depth==0 and i>0 and toks[i-1][1]==')':
                op=mo.get(i-1)
                if op is not None and op>0:
                    name_i=op-1
                    if re.fullmatch(r'[A-Za-z_]\w*', toks[name_i][1]):
                        # find start of declaration: walk back over the
                        # return type's tokens. Stopping at the previous
                        # top-level ';' or '}' is not enough: a file whose
                        # first function follows a block of #defines has
                        # neither, and the type range would then swallow
                        # the preprocessor lines (and the comment above
                        # them) and get them collapsed onto one line.
                        j=name_i-1
                        tstart=name_i
                        while j>=0:
                            if depths_after[j]!=0:
                                break
                            t=toks[j][1]
                            if is_macro[j]:
                                j-=1
                                continue
                            if t=='*' or t in TYPE_TOKENS or IDENT.fullmatch(t):
                                tstart=j
                                j-=1
                                continue
                            break
                        # no function-pointer return; no comment inside range
                        seg=s[(toks[tstart][0] if tstart<name_i else p):(toks[name_i][0] if tstart<name_i else p)]
                        if tstart<name_i and '(' not in [tok for _,tok in toks[tstart:name_i]] \
                           and '/*' not in seg and '//' not in seg:
                            defs.append((tstart,name_i,i,op,i-1))
            depth+=1
        elif t=='}': depth=max(0,depth-1)
    return defs, is_macro

def collapse_ws(inner):
    """Collapse whitespace runs without touching literals or comments.

    Returns None when the list can't be joined safely: line comments would
    swallow whatever follows on the joined line, multi-line comments would
    re-introduce a newline, and preprocessor lines can't be moved.
    """
    parts=[]; literals=[]; last=0
    for m in TOK.finditer(inner):
        t=m.group(0)
        if t.startswith('#'):
            return None
        if t.startswith('//'):
            return None
        if t.startswith('/*') or t[0] in '"\'':
            if '\n' in t:
                return None
            parts.append(inner[last:m.start()])
            parts.append(f'\x00{len(literals)}\x00')
            literals.append(t)
            last=m.end()
    parts.append(inner[last:])
    text=re.sub(r'\s+', ' ', ''.join(parts))
    text=re.sub(r'\s+([,)])', r'\1', text)
    text=re.sub(r'\(\s+', '(', text)
    text=re.sub(r'\s+\)', ')', text)
    for i,t in enumerate(literals):
        text=text.replace(f'\x00{i}\x00', t)
    return text.strip()

def collapse_params(s):
    """Join every wrapped parameter list onto the name line.

    Runs after the return-type pass, so a non-portable return type may
    still be split but name/parameters are always one line.
    """
    toks=tokenize(s); ms=macro_lines(s)
    defs,_=analyze_defs(s,toks,ms)
    edits=[]
    for _,name_i,_,open_i,close_i in defs:
        op=toks[open_i][0]; cp=toks[close_i][0]
        if line_start(s,op)==line_start(s,cp):
            continue
        collapsed=collapse_ws(s[op+1:cp])
        if collapsed is None:
            continue
        edits.append((op+1,cp,collapsed))
    edits.sort(reverse=True)
    for st,en,rep in edits:
        s=s[:st]+rep+s[en:]
    return s, len(edits)

def rewrite(s):
    toks=tokenize(s); ms=macro_lines(s)
    defs,_=analyze_defs(s,toks,ms)
    edits=[]
    for tstart,name_i,brace_i,open_i,close_i in defs:
        if tstart>=name_i: continue
        if any(toks[k][1]=='(' for k in range(tstart,name_i)): continue
        # A pointer return is a declarator, not a base type: keep the `*`
        # with the name on one line rather than splitting `T *` / `name`.
        star_return=any(t=='*' for _,t in toks[tstart:name_i])
        portable=classify_base(s,toks,tstart,name_i) or star_return
        name_pos=toks[name_i][0]
        last=name_i-1
        # collapse type-internal newlines and fix type/name gap
        # first, any newlines within the type range -> single space (or none after '*')
        for k in range(tstart+1,name_i+1):
            prev_end = toks[k-1][0]+len(toks[k-1][1])
            cur = toks[k][0]
            gap = s[prev_end:cur]
            if '\n' not in gap: continue
            if '/*' in gap or '//' in gap:
                continue
            if portable:
                rep = '' if toks[k-1][1]=='*' else ' '
                edits.append((prev_end,cur,rep))
            # non-portable: leave existing split alone
        if not portable:
            # ensure split between last type token and name
            last_end=toks[last][0]+len(toks[last][1])
            gap=s[last_end:name_pos]
            if '\n' not in gap and '/*' not in gap and '//' not in gap:
                edits.append((last_end,name_pos,'\n'+indent_of(s,toks[tstart][0])))
        else:
            # If the parameter list wraps, shift continuation lines by the
            # same amount the open paren moved when the name joined the
            # return-type line.
            close_pos = toks[close_i][0]
            name_line = line_start(s, name_pos)
            if line_start(s, close_pos) != name_line:
                tpos = toks[tstart][0]
                tline = line_start(s, tpos)
                # length of the collapsed prefix from type start through last type token
                seg = s[tpos:toks[last][0]+len(toks[last][1])]
                # apply the same gap-collapse rules used above
                collapsed = []
                cur = tpos
                for k in range(tstart+1, last+1):
                    prev_end = toks[k-1][0]+len(toks[k-1][1])
                    cur_pos = toks[k][0]
                    gap = s[prev_end:cur_pos]
                    if '\n' in gap:
                        collapsed.append(s[cur:prev_end])
                        collapsed.append('' if toks[k-1][1]=='*' else ' ')
                        cur = cur_pos
                collapsed.append(s[cur:toks[last][0]+len(toks[last][1])])
                prefix_len = len(''.join(collapsed))
                sep_len = 0 if toks[last][1]=='*' else 1
                type_col = tpos - tline
                old_name_col = name_pos - name_line
                new_name_col = type_col + prefix_len + sep_len
                delta = new_name_col - old_name_col
                if delta:
                    ls = name_line+1
                    endline = line_start(s, close_pos)
                    while ls <= endline and ls < len(s):
                        le = line_end(s, ls)
                        lead = ls
                        while lead < le and s[lead] in ' \t':
                            lead += 1
                        if lead > ls:
                            if delta > 0:
                                edits.append((ls, ls, ' '*delta))
                            else:
                                edits.append((ls, lead, s[ls:max(ls, lead+delta)]))
                        if le == len(s): break
                        ls = le+1
    edits.sort(); clean=[]; last=-1
    for st,en,rep in edits:
        if st<last: continue
        clean.append((st,en,rep)); last=en
    out,cur=[],0
    for st,en,rep in clean:
        out.append(s[cur:st]); out.append(rep); cur=en
    out.append(s[cur:])
    text, collapsed = collapse_params(''.join(out))
    return text, len(clean)+collapsed, defs

def main():
    apply='--apply' in sys.argv; check='--check' in sys.argv
    files=sorted(set(glob.glob('src/**/*.[ch]',recursive=True)+glob.glob('include/**/*.[ch]',recursive=True)+glob.glob('tools/**/*.[ch]',recursive=True)+glob.glob('tests/**/*.[ch]',recursive=True)))
    total=touched=0
    for f in files:
        src=open(f,encoding='utf-8').read(); new,n,_=rewrite(src)
        if n:
            total+=n; touched+=1
            if apply: open(f,'w',encoding='utf-8').write(new)
    if check and total:
        print(f"FAIL: {total} edits needed in {touched} files"); sys.exit(1)
    print(f"{'APPLIED' if apply else 'DRY RUN'}: {total} edits in {touched} files")

if __name__=='__main__': main()
