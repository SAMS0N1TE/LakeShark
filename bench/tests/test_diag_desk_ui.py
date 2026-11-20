"""DIAG in a desk window, driven as a PC drives it: a menu bar with its own
Tools and View beside the desk's titles, the sections listed down the left
with the open one on the selection bar, a click and the arrow keys moving
between them, the menus down from Alt+T / Alt+V, and the right button's menu.
The page beside the list is the handheld's own, so its blocks must still be
there. Reads the cells lssim prints."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
work = sim.parent / 'diag-desk-ui'
work.mkdir(exist_ok=True)
OPEN = '!diag,^,~3'


def run(name, script, grid='160x48', **env):
    e = dict(os.environ, LSSIM_GRID=grid, **env)
    result = subprocess.run([str(sim), 'desk', '-x', script, '-d', '-o', str(work / (name + '.bmp'))],
                            capture_output=True, text=True, check=True, cwd=work, env=e)
    (work / (name + '.txt')).write_text(result.stdout, encoding='utf-8')
    cells = {}
    for line in result.stdout.splitlines():
        if line[:4].strip().isdigit():
            cells[int(line[:4])] = line[4:]
    return cells


def has(cells, text):
    return any(text in line for line in cells.values())


def row_of(cells, text):
    return next(y for y in sorted(cells) if text in cells[y])


def chosen(cells):
    """The section the list marks."""
    for y in sorted(cells):
        at = cells[y].find('|> ')
        if at >= 0:
            return cells[y][at + 3:at + 23].strip()
    return None


# The layout: the app's bar carries the desk's titles, the list, the page.
c = run('open', OPEN)
bar = row_of(c, ' Tools  View ')
assert 'Desk' in c[bar] and 'Window' in c[bar] and 'Help' in c[bar], c[bar]
assert not has(c, ' File  View  Window  Help'), 'the desk drew a second bar'
assert has(c, 'SECTIONS') and has(c, '1 of 7')
assert chosen(c) == 'Overview', chosen(c)
for name in ('Endpoint', 'Memory', 'Alerts', 'Radios and sensors', 'Error log', 'Receiver trial'):
    assert has(c, name), name
assert has(c, 'P25 CHAIN') and has(c, 'STORAGE AND FIRMWARE'), 'the page lost a block'
assert has(c, '[ TEST ALERT ]'), 'the page lost its keys'

# Arrow keys walk the sections; the page follows.
c = run('down2', OPEN + ',@down,@down,~2')
assert chosen(c) == 'Memory', chosen(c)
assert has(c, '3 of 7') and has(c, 'LARGEST BLOCK')
c = run('updown', OPEN + ',@down,@down,@up,@up,@up,~2')
assert chosen(c) == 'Overview', chosen(c)
c = run('end', OPEN + ',@end,~2')
assert chosen(c) == 'Receiver trial', chosen(c)

# A click on a line of the list opens that section.
y = row_of(run('open2', OPEN), '  Alerts  ')
c = run('click', OPEN + ',C5:%d,~2' % y)
assert chosen(c) == 'Alerts', chosen(c)
assert has(c, '4 of 7')

# The handheld's letters still answer, and Esc is the way back.
c = run('letter', OPEN + ',+m,~2')
assert chosen(c) == 'Memory', chosen(c)
c = run('esc', OPEN + ',+m,@esc,~2')
assert chosen(c) == 'Overview', chosen(c)

# Alt+T: Tools. The log's commands are greyed off the log; Esc closes it.
c = run('tools', OPEN + ',M2,+t,M0,~2')
assert has(c, 'Test alert') and has(c, 'Check the SD card') and has(c, 'Clear the error log...')
c = run('toolsesc', OPEN + ',M2,+t,M0,@esc,~2')
assert not has(c, 'Check the SD card') and chosen(c) == 'Overview'

# Alt+V: View lists the sections, the open one ticked; Enter goes there.
c = run('view', OPEN + ',M2,+v,M0,~2')
assert has(c, 'v Overview') and has(c, 'Receiver trial')
c = run('viewpick', OPEN + ',M2,+v,M0,@down,@down,@down,@enter,~2')
assert chosen(c) == 'Alerts', chosen(c)
# Right from Tools is View.
c = run('across', OPEN + ',M2,+t,M0,@right,~2')
assert has(c, 'v Overview') and not has(c, 'Check the SD card')

# A click on the bar's own title opens its menu too.
x = c[bar].find('View')
c = run('barclick', OPEN + ',C%d:%d,~2' % (x + 1, bar))
assert has(c, 'v Overview')

# The right button, anywhere on the page: sections, then tools.
c = run('context', OPEN + ',R90:20,~2')
assert has(c, '| DIAG') and has(c, 'Check the SD card') and has(c, 'Error log')

# The error log from the list: the page is the log, and Tools offers its commands.
c = run('log', OPEN + ',@down,@down,@down,@down,@down,~2')
assert chosen(c) == 'Error log', chosen(c)
assert has(c, 'ERROR LOG')

# A light scheme: the bar under the open section is one colour end to end.
c = run('classic', OPEN + ',@down,~2', LSSIM_DESK_THEME='classic', LSSIM_CELLS_OUT=str(work / 'classic.cells'))
assert chosen(c) == 'Endpoint', chosen(c)
import struct
d = (work / 'classic.cells').read_bytes()
W = 160
y = row_of(c, '|> Endpoint')
x0 = c[y].find('|> Endpoint') + 1
bgs = {struct.unpack_from('<hB', d, (y * W + x) * 4)[1] >> 4 for x in range(x0, x0 + 24)}
assert len(bgs) == 1, 'holes in the selection bar: %s' % sorted(bgs)
face = struct.unpack_from('<hB', d, ((y + 1) * W + x0 + 3) * 4)[1] >> 4
assert bgs != {face}, 'the open section has no bar'

# A narrow window keeps the handheld page whole (no list, no bar of its own).
c = run('narrow', '!diag,~3', grid='120x40')
if not has(c, 'SECTIONS'):
    assert has(c, 'RADIO') or has(c, 'MEMORY')

print('PASS: DIAG desk layout')
