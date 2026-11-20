"""Source lint for module statics (used by build_modules.py).

On the T-Display a module's statics live in the module's PSRAM image, but on the Nano modules are
compiled into the firmware, so a big static/global lands in internal RAM and can break the boot
reservation (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL needs a contiguous block at main_task).
Any module static/global variable larger than RAM_LIMIT bytes must carry EXT_RAM_BSS_ATTR, or a
"mod-lint: ok <why>" comment on its line / the line above once a human has reviewed it.
Sizes are estimated from the source (primitive types, local structs, #define dims); a
declaration whose size cannot be worked out is skipped, bench/ramgate.py checks the real build.
"""
import re
from pathlib import Path

RAM_LIMIT = 2048

_PRIM = {'char': 1, 'int8_t': 1, 'uint8_t': 1, 'bool': 1, '_Bool': 1, 'short': 2, 'int16_t': 2, 'uint16_t': 2,
         'int': 4, 'long': 4, 'float': 4, 'int32_t': 4, 'uint32_t': 4, 'size_t': 4, 'ssize_t': 4,
         'intptr_t': 4, 'uintptr_t': 4, 'ptrdiff_t': 4, 'TickType_t': 4, 'esp_err_t': 4, 'wchar_t': 4,
         'double': 8, 'int64_t': 8, 'uint64_t': 8, 'time_t': 8, 'long long': 8}
_QUAL = r'\b(static|volatile|register|extern|const|signed|inline|restrict)\b'


def _strip(text):
    """Blank out comments and string/char literals, keeping newlines."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * max(0, j - i - 1) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


_INDEX = {}
_CACHE = {}


def _header_index(root):
    if root not in _INDEX:
        idx = {}
        for sub in ('components', 'main'):
            for h in (Path(root) / sub).rglob('*.h'):
                idx.setdefault(h.name, h)
        _INDEX[root] = idx
    return _INDEX[root]


class Env:
    """#defines and struct sizes visible to one source (itself plus its local quoted includes)."""

    def __init__(self, path, root=None, seen=None, depth=0):
        self.defs, self.structs, self.anon = {}, {}, 0
        raw = Path(path).read_text(encoding='utf-8', errors='replace')
        self.text = _strip(raw)
        seen = seen if seen is not None else set()
        seen.add(Path(path).resolve())
        idx = _header_index(root) if root else {}
        if depth < 6:
            for inc in re.findall(r'^\s*#\s*include\s+["<]([^">]+)[">]', raw, re.M):
                cp = Path(path).parent / inc
                if not cp.is_file():
                    cp = idx.get(Path(inc).name)
                if cp is not None and cp.resolve() not in seen:
                    key = cp.resolve()
                    if key not in _CACHE:
                        sub = Env(cp, root, seen, depth + 1)
                        _CACHE[key] = (sub.defs, sub.structs)
                    self.defs.update(_CACHE[key][0])
                    self.structs.update(_CACHE[key][1])
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$', self.text, re.M):
            self.defs[m.group(1)] = m.group(2)
        self._collect_structs()

    def _collect_structs(self):
        text = self.text
        pat = re.compile(r'(typedef\s+)?\b(struct|union)\s*(\w*)\s*\{([^{}]*)\}\s*(\w*)\s*;')

        def repl(m):
            td, kind, tag, body, var = m.groups()
            sz = self.body_size(body, kind == 'union')
            if sz is None:
                return ' ' * 0 + ' '
            if td:
                self.structs[var or tag] = sz
                if tag:
                    self.structs[kind + ' ' + tag] = sz
                return ' '
            if tag:
                self.structs[kind + ' ' + tag] = sz
                return (kind + ' ' + tag + ' ' + var + ';') if var else ' '
            self.anon += 1
            nm = '__anon%d' % self.anon
            self.structs[nm] = sz
            return nm + ' ' + var + ';'

        for _ in range(5):
            new = pat.sub(repl, text)
            if new == text:
                break
            text = new

    def body_size(self, body, union=False):
        total, mx = 0, 1
        for st in body.split(';'):
            st = st.strip()
            if not st:
                continue
            if '(' in st:
                continue                    # function pointer member: a pointer
            if re.search(r':\s*\d+\s*$', st):
                total += 4
                continue
            r = self.decl_size(st)
            if r is None:
                return None
            sz, al = r
            mx = max(mx, al)
            total = max(total, sz) if union else (total + al - 1) // al * al + sz
        return (total + mx - 1) // mx * mx

    def ev(self, expr, depth=0):
        if depth > 8:
            return None
        expr = re.sub(r'\(\s*(?:unsigned|int|size_t|uint\d+_t)\s*\)', '', expr.strip())
        expr = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', expr)

        def sub(m):
            w = m.group(0)
            d = self.defs.get(w)
            if d is not None and not w.startswith('0'):
                v = self.ev(d, depth + 1)
                return str(v) if v is not None else '?'
            return w if re.fullmatch(r'0[xX][0-9a-fA-F]+', w) else '?'
        expr = re.sub(r'\b0[xX][0-9a-fA-F]+\b|\b[A-Za-z_]\w*\b', sub, expr)
        if '?' not in expr and re.fullmatch(r'[\d\s+\-*/()<>|&xXa-fA-F%]*', expr):
            try:
                return int(eval(expr.replace('/', '//'), {'__builtins__': {}}))
            except Exception:
                return None
        return None

    def decl_size(self, st):
        """(size, align) of the first declarator of a declaration, or None when unknown."""
        st = re.sub(r'=.*', '', st, flags=re.S)
        st = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', st)
        st = re.sub(_QUAL, ' ', st)
        st = re.sub(r'\b\w*_ATTR\b', ' ', st)
        parts = st.split(',')
        extra = []
        for p in parts[1:]:                         # "uint8_t kind, sec[4]": every declarator counts
            pm = re.match(r'^\s*(\**)\s*\w+\s*((?:\[[^\]]*\]\s*)*)$', p)
            if not pm:
                return None
            k = 1
            for d in re.findall(r'\[([^\]]*)\]', pm.group(2)):
                v = self.ev(d)
                if v is None:
                    return None
                k *= v
            extra.append((bool(pm.group(1)), k))
        st = parts[0]
        dims = re.findall(r'\[([^\]]*)\]', st)
        head = re.sub(r'\[[^\]]*\]', '', st).strip()
        m = re.match(r'^(.*?)(\**)\s*(\w+)\s*$', head, re.S)
        if not m:
            return None
        ty = ' '.join(m.group(1).split())
        stars = m.group(2) or ('*' if ty.endswith('*') else '')
        ty = re.sub(r'(unsigned|signed)', '', ty).strip() or 'int'
        if stars:
            base, al = 4, 4
        elif ty in _PRIM:
            base = _PRIM[ty]
            al = min(base, 8)
        elif ty in self.structs:
            base = self.structs[ty]
            al = 4 if base >= 4 else max(1, base)
        elif re.sub(r'^(struct|union|enum)\s+', '', ty) in self.structs:
            base = self.structs[re.sub(r'^(struct|union)\s+', '', ty)]
            al = 4
        elif ty.startswith('enum'):
            base = al = 4
        else:
            return None
        n = 1
        for d in dims:
            v = self.ev(d)
            if v is None:
                return None
            n *= v
        return base * n + sum((4 if ptr else base) * k for ptr, k in extra), al


def _statements(text):
    """Yield (first_line, text) of each file-scope declaration statement (function bodies dropped)."""
    depth, buf, start_line, line = 0, [], None, 1
    keep = 0                                   # depth at which an aggregate/initializer body is part of the statement
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
        if depth == 0 and c == '#':             # preprocessor line: skip it (and continuations)
            j = i
            while j < n:
                k = text.find('\n', j)
                k = n if k < 0 else k
                cont = text[j:k].rstrip().endswith('\\')
                j = k + 1
                line += 1
                if not cont:
                    break
            i = j
            continue
        if c == '{':
            joined = ''.join(buf).strip()
            if depth == 0 and not keep:
                if re.search(r'=\s*$', joined) or re.match(r'^(typedef\b|(struct|union|enum)\b)', joined) \
                        and '(' not in joined:
                    keep = 1
                elif re.fullmatch(r'extern\s*"\s*"', joined):
                    buf, start_line = [], None
                    i += 1
                    continue
            depth += 1
            if keep:
                buf.append('{')
        elif c == '}':
            if depth == 0:                      # closing an extern "C" block
                i += 1
                continue
            depth -= 1
            if keep:
                buf.append('}')
                if depth == 0:
                    keep = 0
            elif depth == 0:                    # function body done
                buf, start_line = [], None
        elif depth == 0 or keep:
            if c == ';' and depth == 0:
                if start_line is not None:
                    yield start_line, ''.join(buf)
                buf, start_line = [], None
            else:
                if start_line is None and not c.isspace():
                    start_line = line
                buf.append(c)
        i += 1


def lint_ram(src_path, rel, mid, raw_lines, root=None):
    """-> ["file:line: message"] for module statics above RAM_LIMIT without EXT_RAM_BSS_ATTR."""
    bad = []
    env = Env(src_path, root)
    cands = {}
    for ln, stmt in _statements(env.text):
        cands[ln] = stmt
    for m in re.finditer(r'\bstatic\b([^=;(){}]*)([=;({])', env.text):      # file-scope and function-local statics
        if m.group(2) in '({':
            continue
        ln = env.text.count(chr(10), 0, m.start()) + 1
        cut = max(env.text.rfind(c, 0, m.start()) for c in ';{}' + chr(10))      # attributes may come before "static"
        cands.setdefault(ln, env.text[cut + 1:m.start()] + 'static' + m.group(1))
    for ln, stmt in sorted(cands.items()):
        s = ' '.join(stmt.split())
        if not s or re.match(r'^(typedef|extern)\b', s):
            continue
        if 'EXT_RAM_BSS_ATTR' in s or '.ext_ram' in s or 'static_assert' in s.lower():
            continue
        head = s.split('=')[0]
        if re.search(r'\bconst\b', head):
            after = head.split('*')[-1]
            if '*' not in head or re.search(r'\bconst\b', after):
                continue                        # read-only data stays in flash
        if re.search(r'\w\s*\(', head) and not re.search(r'\(\s*\*', head):
            continue                            # function declaration
        flat = re.sub(r'\{.*\}', '{}', s)       # drop aggregate/initializer bodies
        if re.match(r'^(struct|union|enum)\s*\w*\s*\{\}\s*;?$', flat):
            continue
        # a struct defined and declared in one statement: size it from the body
        m = re.match(r'^(?:static\s+)?(struct|union)\s*(\w*)\s*\{(.*)\}\s*([\w\[\]\s*]+)$', s)
        if m:
            bsz = env.body_size(m.group(3), m.group(1) == 'union')
            if bsz is None:
                continue
            n = 1
            for d in re.findall(r'\[([^\]]*)\]', m.group(4)):
                v = env.ev(d)
                n = n * v if v is not None else 0
            r = (bsz * n, 4)
        else:
            r = env.decl_size(flat)
        if r is None or r[0] <= RAM_LIMIT:
            continue
        near = ' '.join(raw_lines[max(0, ln - 2):ln + 1])
        if 'mod-lint: ok' in near:
            continue
        bad.append('%s:%d: [%s] ~%d-byte static/global without EXT_RAM_BSS_ATTR: modules are compiled into the Nano '
                   'firmware, where this lands in internal RAM (add EXT_RAM_BSS_ATTR, or "mod-lint: ok <why>")'
                   % (rel, ln, mid, r[0]))
    return bad
