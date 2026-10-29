"""Build-time text packs: long read-only text moved out of the image's
string literals into zstd frames the firmware decodes on first use.

Two jobs, both run by the build (components/ls_textpack, components/apps):

  text  OUT_DIR SRC_DIR...
        Finds every LS_TEXT(id, "...") and LS_TXT(id, "...") in the C/C++
        sources under SRC_DIR, and writes
          OUT_DIR/ls_textpack_ids.h    enum LS_TXT_<id>, LS_TEXT_COUNT
          OUT_DIR/ls_textpack_data.c  the frames and the id -> text index
          OUT_DIR/ls_textpack_src.c   (--src-table) every literal as written, for
                                   the bench's round-trip test
  i18n  TABLE OUT_C OUT_H
        Reads ls_i18n_uk.h's { "English", "Ukrainian" } pairs and writes the
        same pairs, in order, as frames (ls_i18n.c's LS_I18N_PACKED build).

Authors keep writing plain string literals; the literal is parsed here with
C's rules (adjacent literals join, escapes as the compiler reads them) and the
round-trip test compares what the firmware decodes with what the compiler
made of the very same tokens.

Outputs are rewritten only when their bytes change, so an edit that moves no
text rebuilds nothing.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lszstd  # noqa: E402

FRAME_MAX = 60 * 1024      # under CartoCore's 64 KiB CZ7 window
# macro -> arguments between the id and the literal
MACROS = {'LS_TEXT': 0, 'LS_TXT': 0, 'LS_PRINTF': 0, 'LS_FPRINTF': 1, 'LS_SNPRINTF': 2,
          'LS_PUTS': 0, 'LS_FPUTS': 0, 'LS_SNPUTS': 2}
SKIP_FILES = {'ls_textpack.h'}
EXTS = ('.c', '.cpp', '.cc', '.h', '.hpp', '.inc')


class GenError(Exception):
    pass


# ------------------------------------------------------------ C lexing ---

_ESC = {'n': 10, 't': 9, 'r': 13, '0': 0, 'a': 7, 'b': 8, 'f': 12, 'v': 11,
        '\\': 92, '"': 34, "'": 39, '?': 63}


def unescape(body, where):
    """Bytes of one C string literal's body (between the quotes)."""
    out = bytearray()
    raw = body.encode('utf-8')
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c != 0x5C:
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            raise GenError('%s: backslash at end of literal' % where)
        e = chr(raw[i])
        if e in '01234567':
            j = i
            v = 0
            while j < n and j < i + 3 and chr(raw[j]) in '01234567':
                v = v * 8 + int(chr(raw[j]))
                j += 1
            if v > 255:
                raise GenError('%s: octal escape out of range' % where)
            out.append(v)
            i = j
        elif e == 'x':
            j = i + 1
            while j < n and chr(raw[j]) in '0123456789abcdefABCDEF':
                j += 1
            if j == i + 1:
                raise GenError('%s: \\x with no digits' % where)
            v = int(raw[i + 1:j].decode(), 16)
            if v > 255:
                raise GenError('%s: hex escape out of range' % where)
            out.append(v)
            i = j
        elif e in 'uU':
            k = 4 if e == 'u' else 8
            digits = raw[i + 1:i + 1 + k].decode('ascii', 'replace')
            if len(digits) != k or any(c not in '0123456789abcdefABCDEF' for c in digits):
                raise GenError('%s: \\%s needs %d hex digits' % (where, e, k))
            out += chr(int(digits, 16)).encode('utf-8')   # GCC's narrow charset is UTF-8
            i += 1 + k
        elif e in _ESC:
            out.append(_ESC[e])
            i += 1
        else:
            raise GenError('%s: escape \\%s is not supported here' % (where, e))
    return bytes(out)


_TOKEN = re.compile(r'''
    (?P<ws>[ \t\r\f\v]+)
  | (?P<nl>\n)
  | (?P<lcom>//[^\n]*)
  | (?P<bcom>/\*.*?\*/)
  | (?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>'(?:[^'\\\n]|\\.)*')
  | (?P<id>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<num>\.?[0-9](?:[A-Za-z0-9_.]|[eEpP][+-])*)
  | (?P<pp>\#)
  | (?P<op>.)
''', re.S | re.X)


def tokens(text):
    """(kind, value, line, at_line_start) for the tokens that matter."""
    line = 1
    bol = True
    pos = 0
    n = len(text)
    while pos < n:
        m = _TOKEN.match(text, pos)
        if not m:
            raise GenError('lexer stuck at line %d' % line)
        kind = m.lastgroup
        val = m.group(kind)
        pos = m.end()
        if kind == 'nl':
            line += 1
            bol = True
            continue
        if kind == 'bcom':
            line += val.count('\n')
            continue
        if kind in ('ws', 'lcom'):
            continue
        yield kind, val, line, bol
        bol = False


def literal_run(toks, i, where):
    """Join adjacent string literals from toks[i]; returns (bytes, source text, next i)."""
    parts = []
    srcs = []
    while i < len(toks) and toks[i][0] == 'str':
        v = toks[i][1]
        if not v.startswith('"'):
            raise GenError('%s: only plain "..." literals are packed' % where)
        parts.append(unescape(v[1:-1], where))
        srcs.append(v)
        i += 1
    if not parts:
        raise GenError('%s: expected a string literal' % where)
    return b''.join(parts), ' '.join(srcs), i


def scan_file(path, rel):
    """[(id, bytes, source tokens, rel, line)] for one file."""
    with open(path, encoding='utf-8', errors='strict') as f:
        text = f.read()
    if not any(m in text for m in MACROS):
        return []
    toks = list(tokens(text))
    found = []
    in_define_line = None
    for i, (kind, val, line, bol) in enumerate(toks):
        if kind == 'pp' and bol and i + 1 < len(toks) and toks[i + 1][1] == 'define':
            in_define_line = line
        if kind != 'id' or val not in MACROS:
            continue
        where = '%s:%d' % (rel, line)
        if i + 1 >= len(toks) or toks[i + 1][1] != '(':
            continue    # a mention, not a use (e.g. #ifdef LS_TEXT)
        if in_define_line == line:
            continue    # a macro's own definition, or one built on it
        if not (i + 3 < len(toks) and toks[i + 2][0] == 'id' and toks[i + 3][1] == ','):
            raise GenError('%s: %s(id, ...) expected' % (where, val))
        ident = toks[i + 2][1]
        # skip the arguments that come before the literal (LS_SNPRINTF's
        # buffer and size, LS_FPRINTF's stream)
        j = i + 4
        for _ in range(MACROS[val]):
            depth = 0
            while j < len(toks):
                t = toks[j][1]
                if t in '([{':
                    depth += 1
                elif t in ')]}':
                    if depth == 0:
                        raise GenError('%s: %s(%s, ...) is missing arguments' % (where, val, ident))
                    depth -= 1
                elif t == ',' and depth == 0:
                    break
                j += 1
            j += 1
        if j >= len(toks) or toks[j][0] != 'str':
            raise GenError('%s: %s(%s, ...) needs a string literal there' % (where, val, ident))
        data, src, j = literal_run(toks, j, where)
        closer = ')' if val in ('LS_TEXT', 'LS_TXT') else (',', ')')
        if j >= len(toks) or toks[j][1] not in closer:
            raise GenError('%s: %s(%s, ...) must be one string literal (adjacent ones join)'
                           % (where, val, ident))
        if b'\0' in data:
            raise GenError('%s: %s(%s) holds a NUL' % (where, val, ident))
        found.append((ident, data, src, rel, line))
    return found


def scan(dirs, root):
    texts = {}
    order = []
    for d in dirs:
        walk = [(os.path.dirname(d), [], [os.path.basename(d)])] if os.path.isfile(d) else os.walk(d)
        for base, subdirs, files in walk:
            subdirs.sort()
            for name in sorted(files):
                if not name.endswith(EXTS) or name in SKIP_FILES:
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, root).replace('\\', '/')
                for ident, data, src, r, line in scan_file(path, rel):
                    if ident in texts:
                        if texts[ident][0] != data:
                            raise GenError('%s:%d: LS_TEXT id %s already holds other text (%s:%d)'
                                           % (r, line, ident, texts[ident][2], texts[ident][3]))
                        continue
                    texts[ident] = (data, src, r, line)
                    order.append(ident)
    return texts, order


# -------------------------------------------------------------- packing ---

def make_frames(items):
    """items: list of byte strings, each kept whole in one frame.
    Returns (frames as raw bytes, [(frame, offset)] per item)."""
    frames = []
    place = []
    cur = bytearray()
    for data in items:
        need = len(data) + 1
        if need > FRAME_MAX:
            raise GenError('one text of %d bytes is over a frame' % len(data))
        if len(cur) + need > FRAME_MAX:
            frames.append(bytes(cur))
            cur = bytearray()
        place.append((len(frames), len(cur)))
        cur += data + b'\0'
    if cur:
        frames.append(bytes(cur))
    return frames, place


def encode_frame(raw):
    """CZ7 frame, or the raw bytes when CZ7 cannot hold them or is no smaller."""
    if len(raw) >= 32:
        z = lszstd.cz7(raw)
        if len(z) < len(raw):
            check = lszstd.decode(z[8:])
            if check is not None and check != raw:
                raise GenError('libzstd disagrees with the encoder')
            return z
    return raw


def c_bytes(data, indent='    '):
    lines = []
    for i in range(0, len(data), 20):
        lines.append(indent + ','.join('0x%02x' % b for b in data[i:i + 20]) + ',')
    return '\n'.join(lines)


def frames_c(prefix, frames):
    out = []
    enc = [encode_frame(f) for f in frames]
    for k, z in enumerate(enc):
        out.append('static const uint8_t %s_f%d[%d] = {\n%s\n};' % (prefix, k, len(z), c_bytes(z)))
    out.append('const ls_text_frame_t %s_frames[%d] = {' % (prefix, max(len(enc), 1)))
    for k, z in enumerate(enc):
        out.append('    { %s_f%d, %d, %d },' % (prefix, k, len(z), len(frames[k])))
    if not enc:
        out.append('    { 0, 0, 0 },')
    out.append('};')
    out.append('const unsigned %s_nframes = %d;' % (prefix, len(enc)))
    out.append('const char *%s_slots[%d];' % (prefix, max(len(enc), 1)))
    return '\n'.join(out), sum(len(z) for z in enc), sum(len(f) for f in frames)


def write_if_changed(path, text):
    data = text.encode('utf-8')
    try:
        with open(path, 'rb') as f:
            if f.read() == data:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)
    return True


HEAD = '/* Generated by components/ls_textpack/gen/textpack_gen.py. Do not edit. */\n'


def job_text(out_dir, dirs, root, src_table):
    texts, order = scan(dirs, root)
    if len(texts) >= 4095:
        raise GenError('%d texts: LS_TXT tags stop at 4095 (ls_textpack.h LS_TEXT_TAG_SPAN)' % len(texts))
    # Frames hold texts used together: by the id's first word (con_ console
    # help, doc_ app docs, ...), then where they are written.
    order.sort(key=lambda t: (t.split('_')[0], texts[t][2], texts[t][3]))
    # Same bytes under two ids are stored once.
    uniq = []
    slot = {}
    for ident in order:
        data = texts[ident][0]
        if data not in slot:
            slot[data] = len(uniq)
            uniq.append(data)
    frames, place = make_frames(uniq)
    # Enum order follows the frames (then the offset, then the name), so a
    # text's frame is the range its id falls in and the index needs only the
    # 16-bit offset (a frame is under 64 KiB): 2 bytes an id, not 4.
    ids = sorted(texts, key=lambda t: (place[slot[texts[t][0]]], t))
    first = []
    for k, ident in enumerate(ids):
        fr = place[slot[texts[ident][0]]][0]
        while len(first) <= fr:
            first.append(k)
    h = [HEAD, '#pragma once', '#define LS_TEXT_COUNT %d' % len(ids), 'enum {']
    for k, ident in enumerate(ids):
        h.append('    LS_TXT_%s = %d,' % (ident, k))
    h.append('    LS_TXT__END')
    h.append('};')
    write_if_changed(os.path.join(out_dir, 'ls_textpack_ids.h'), '\n'.join(h) + '\n')
    body, zbytes, rbytes = frames_c('ls_text', frames)
    c = [HEAD, '#include "ls_textpack.h"', '', body, '',
         'const unsigned ls_text_count = %d;' % len(ids),
         '/* the first id of each frame */',
         'const uint16_t ls_text_frame_first[%d] = { %s };'
         % (max(len(first), 1), ', '.join(str(f) for f in first) or '0'),
         '/* offset in its frame, by id */',
         'const uint16_t ls_text_index[%d] = {' % max(len(ids), 1)]
    for ident in ids:
        fr, off = place[slot[texts[ident][0]]]
        if off > 0xFFFF:
            raise GenError('text %s at offset %d: frames must stay under 64 KiB' % (ident, off))
        c.append('    %d, /* %s */' % (off, ident))
    if not ids:
        c.append('    0,')
    c.append('};')
    c.append('/* %d texts, %d bytes as text, %d as frames */' % (len(ids), rbytes, zbytes))
    write_if_changed(os.path.join(out_dir, 'ls_textpack_data.c'), '\n'.join(c) + '\n')
    if src_table:
        s = [HEAD, '/* Every packed literal exactly as its source wrote it, for the bench. */',
             '#include "ls_textpack_ids.h"', '',
             'const char *const ls_text_src[%d] = {' % max(len(ids), 1)]
        for ident in ids:
            data, src, rel, line = texts[ident]
            s.append('    /* %s:%d */ %s,' % (rel, line, src))
        if not ids:
            s.append('    "",')
        s.append('};')
        s.append('const char *const ls_text_names[%d] = {' % max(len(ids), 1))
        for ident in ids:
            s.append('    "%s",' % ident)
        if not ids:
            s.append('    "",')
        s.append('};')
        write_if_changed(os.path.join(out_dir, 'ls_textpack_src.c'), '\n'.join(s) + '\n')
    print('ls_textpack: %d texts, %d bytes -> %d in %d frame(s)' % (len(ids), rbytes, zbytes, len(frames)))


def parse_i18n(path):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    m = re.search(r'ls_i18n_pair_t\s+UK\s*\[\s*\]\s*=\s*\{', text)
    if not m:
        raise GenError('%s: no ls_i18n_pair_t UK[] table' % path)
    start_line = text.count('\n', 0, m.end()) + 1
    toks = list(tokens(text[m.end():]))
    pairs = []
    i = 0
    while True:
        if i >= len(toks):
            raise GenError('%s: table not closed' % path)
        t = toks[i]
        where = '%s:%d' % (os.path.basename(path), start_line + t[2] - 1)
        if t[1] == '}':
            break
        if t[1] != '{':
            raise GenError('%s: expected {' % where)
        en, en_src, i = literal_run(toks, i + 1, where)
        if toks[i][1] != ',':
            raise GenError('%s: expected ,' % where)
        uk, _, i = literal_run(toks, i + 1, where)
        if toks[i][1] != '}':
            raise GenError('%s: expected }' % where)
        i += 1
        if toks[i][1] == ',':
            i += 1
        if b'\0' in en or b'\0' in uk:
            raise GenError('%s: NUL in an entry' % where)
        pairs.append((en, uk, en_src))
    return pairs


def job_i18n(table, out_c, out_h):
    """English keys stay literals (nearly all are the interface's own labels,
    so the linker shares them with the code); the Ukrainian goes to frames."""
    pairs = parse_i18n(table)
    frames, _ = make_frames([uk for _, uk, _ in pairs])
    body, zbytes, rbytes = frames_c('ls_i18n', frames)
    write_if_changed(out_h, '\n'.join([
        HEAD, '#pragma once', '#include "ls_textpack.h"',
        '#define LS_I18N_PAIRS %d' % len(pairs),
        '/* the English keys, in table order */',
        'extern const char *const ls_i18n_en[LS_I18N_PAIRS];',
        '/* frames of "Ukrainian\\0" strings, in table order */',
        'extern const ls_text_frame_t ls_i18n_frames[];',
        'extern const unsigned ls_i18n_nframes;',
        'extern const char *ls_i18n_slots[];', '']))
    en = ['const char *const ls_i18n_en[LS_I18N_PAIRS] = {']
    en += ['    %s,' % src for _, _, src in pairs]
    en.append('};')
    write_if_changed(out_c, '\n'.join([HEAD, '#include "%s"' % os.path.basename(out_h), '',
                                       '\n'.join(en), '', body,
                                       '/* %d pairs; Ukrainian %d bytes as text, %d as frames */'
                                       % (len(pairs), rbytes, zbytes), '']))
    print('ls_i18n: %d pairs, %d bytes -> %d in %d frame(s)' % (len(pairs), rbytes, zbytes, len(frames)))


_BYTES_ARRAY = re.compile(
    r'static\s+const\s+uint8_t\s+(?P<name>\w+)\s*\[\s*(?P<n>\d*)\s*\]\s*=\s*\{(?P<body>[^}]*)\};')


def job_fonts(out_dir, sources):
    """Glyph bitmaps (and any other big static const uint8_t table in these
    files) moved to one PSRAM array per file, filled from a frame by a
    constructor before app_main. Each output is the source as written with
    only those definitions replaced; every use of a table becomes the same
    address inside the PSRAM array, so the font structs stay constant."""
    for src in sources:
        with open(src, encoding='utf-8') as f:
            text = f.read()
        tables = []
        for m in _BYTES_ARRAY.finditer(text):
            body = re.sub(r'/\*.*?\*/', '', m.group('body'), flags=re.S)
            vals = [int(v, 0) for v in re.findall(r'0[xX][0-9a-fA-F]+|\d+', body)]
            if any(v > 255 for v in vals):
                raise GenError('%s: %s holds a value over 255' % (src, m.group('name')))
            if m.group('n') and int(m.group('n')) != len(vals):
                raise GenError('%s: %s[%s] has %d values' % (src, m.group('name'), m.group('n'), len(vals)))
            if len(vals) >= 512:
                tables.append((m, bytes(vals)))
        if not tables:
            raise GenError('%s: no table to pack' % src)
        stem = re.sub(r'\W', '_', os.path.splitext(os.path.basename(src))[0])
        arr = 'lsfp_%s' % stem
        raw = b''.join(t for _, t in tables)
        if len(raw) > lszstd.MAX_FRAME:
            raise GenError('%s: %d bytes of tables is over a frame' % (src, len(raw)))
        out = []
        pos = 0
        offsets = {}
        off = 0
        for m, data in tables:
            name = m.group('name')
            if re.search(r'sizeof\s+%s\b' % re.escape(name), text):
                raise GenError('%s: write sizeof(%s) with parentheses' % (src, name))
            offsets[name] = (off, len(data))
            out.append(text[pos:m.start()])
            out.append('/* @@%d@@: %d bytes, in %s (filled at start-up) */'
                       % (len(offsets) - 1, len(data), arr))
            pos = m.end()
            off += len(data)
        out.append(text[pos:])
        body = ''.join(out)
        for name, (o, n) in offsets.items():
            body = re.sub(r'sizeof\s*\(\s*%s\s*\)' % re.escape(name), '((size_t)%d)' % n, body)
            body = re.sub(r'(?<![.\w])%s(?!\w)' % re.escape(name), '(%s + %d)' % (arr, o), body)
        for k, name in enumerate(offsets):
            body = body.replace('@@%d@@' % k, name)
        z = encode_frame(raw)
        head = (HEAD + '/* %s with its tables (%d bytes) packed to %d bytes. */\n'
                '#include <stddef.h>\n#include <stdint.h>\n#include "esp_attr.h"\n'
                '#include "ls_textpack.h"\n'
                'EXT_RAM_BSS_ATTR static uint8_t %s[%d];\n'
                % (os.path.basename(src), len(raw), len(z), arr, len(raw)))
        tail = ('\nstatic const uint8_t %s_z[%d] = {\n%s\n};\n'
                'static const ls_text_frame_t %s_frame = { %s_z, %d, %d };\n'
                '/* Before app_main (PSRAM and the heap are up by then), so the\n'
                '   splash and every later draw find the bitmaps in place. */\n'
                '__attribute__((constructor)) static void %s_unpack(void)\n'
                '{\n    (void)ls_text_unpack_into(&%s_frame, %s, %d);\n}\n'
                % (arr, len(z), c_bytes(z), arr, arr, len(z), len(raw), arr, arr, arr, len(raw)))
        write_if_changed(os.path.join(out_dir, os.path.basename(src)), head + body + tail)
        print('ls_fontpack: %s %d bytes -> %d' % (os.path.basename(src), len(raw), len(z)))


def main(argv):
    try:
        if len(argv) >= 3 and argv[1] == 'text':
            args = argv[2:]
            src_table = '--src-table' in args
            args = [a for a in args if a != '--src-table']
            root = None
            if args and args[0].startswith('--root='):
                root = args[0][7:]
                args = args[1:]
            out_dir, dirs = args[0], args[1:]
            job_text(out_dir, dirs, root or os.path.commonpath([os.path.abspath(d) for d in dirs]),
                     src_table)
        elif len(argv) == 5 and argv[1] == 'i18n':
            job_i18n(argv[2], argv[3], argv[4])
        elif len(argv) >= 4 and argv[1] == 'fonts':
            job_fonts(argv[2], argv[3:])
        else:
            print(__doc__)
            return 2
    except GenError as e:
        print('textpack_gen: error: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
