import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding='ascii').read()
out = ['/* BLE EXPLORER\'s built-in flows: flows/ble-flows.txt as a string. The',
       '   bench test checks the two match; change the .txt, then this. */',
       '#include <stddef.h>',
       '',
       'const char bx_flows_builtin[] =']
for line in text.splitlines(True):
    esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    out.append('    "%s"' % esc)
out[-1] += ';'
out.append('const size_t bx_flows_builtin_len = sizeof(bx_flows_builtin) - 1;')
open(dst, 'w', encoding='ascii', newline='\n').write('\n'.join(out) + '\n')
