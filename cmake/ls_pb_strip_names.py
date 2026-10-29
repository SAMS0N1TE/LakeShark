"""Copy a protobuf-c generated .pb-c.c with its reflection names blank.

protobuf-c keeps the name of every message, field and enum value in its
descriptors. Packing and unpacking go by number; the names serve only the
by-name lookups and the unpack error macro, which esp_hosted builds empty.
Every string literal outside preprocessor lines becomes "", so the linker
keeps one empty string in place of ~40 KB of names. One enum value table can
keep its value names (the second literal on each line, the C name, still goes):
esp_hosted's rpc_core.c logs RpcId names.

    python ls_pb_strip_names.py <in.pb-c.c> <out.pb-c.c> <keep_values_array>
"""
import os
import re
import sys

LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def main(src, dst, keep):
    with open(src, encoding='utf-8') as f:
        lines = f.read().splitlines(keepends=True)
    start = re.compile(r'^static const ProtobufCEnumValue %s\[' % re.escape(keep))
    out, in_keep, found, blanked, kept = [], False, False, 0, 0
    for line in lines:
        if line.lstrip().startswith('#'):
            out.append(line)
            continue
        if start.match(line):
            in_keep = found = True
        if in_keep:
            hits = list(LITERAL.finditer(line))
            if hits:
                kept += 1
                for m in reversed(hits[1:]):
                    line = line[:m.start()] + '""' + line[m.end():]
                    blanked += 1
            if line.startswith('};'):
                in_keep = False
        else:
            line, n = LITERAL.subn('""', line)
            blanked += n
        out.append(line)
    if not found or kept < 10 or blanked < 1000:
        sys.exit(f'{src}: expected {keep} and generated names (kept {kept}, blanked {blanked})')
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    text = ''.join(out)
    if os.path.exists(dst):
        with open(dst, encoding='utf-8') as f:
            if f.read() == text:
                return
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


if __name__ == '__main__':
    main(*sys.argv[1:4])
