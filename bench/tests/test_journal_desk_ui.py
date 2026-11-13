"""JOURNAL in a desk window, driven as a PC drives it: the entries in a table
with the one chosen read beside it, a header clicked to sort, Find over
titles and notes, the right button on an entry, the list saved as text, and
the handheld's live attachments and layout still a key away. Reads the cells
lssim prints and the file the click leaves on the card."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
work = sim.parent / 'journal-desk-ui'
work.mkdir(exist_ok=True)


def run(name, script, grid='160x48', **env):
    e = dict(os.environ, LSSIM_GRID=grid, LSSIM_JOURNAL='14', **env)
    result = subprocess.run([str(sim), 'desk', '-x', script, '-d', '-o', str(work / (name + '.bmp'))],
                            capture_output=True, text=True, check=True, cwd=work, env=e)
    (work / (name + '.txt')).write_text(result.stdout, encoding='utf-8')
    cells = {}
    for line in result.stdout.splitlines():
        if line[:4].strip().isdigit():
            cells[int(line[:4])] = line[4:]
    return cells


def row_of(cells, text, after=0):
    return next(y for y in sorted(cells) if y >= after and text in cells[y])


def has(cells, text):
    return any(text in line for line in cells.values())


OPEN = '!journal,^,~10'

# The window: menus, a toolbar, the table, the entry being read, the CSV's status.
c = run('open', OPEN)
assert ' Journal  View  Entry' in c[1] and '14 entries' in c[1]
assert '[New]' in c[2] and '[Record OFF]' in c[2] and 'Find [' in c[2] and '[Save as text]' in c[2]
top = row_of(c, 'ENTRIES 14 of 14')
assert 'Ridge trail / north overlook' in c[top] and 'on the card' in c[top]
head = top + 1
assert all(t in c[head] for t in ('TITLE', 'WHEN', 'RADIO', 'MHz', 'PLACE'))
assert c[head + 1].lstrip('|').startswith('>Ridge trail')
assert has(c, 'Mesh reception returned near the overlook.') and has(c, '43.200000, -71.650000')
assert 'CSV STOPPED' in c[44]
title_x = c[head].index('TITLE')

# Down moves the cursor and the pane reads that entry.
c = run('down', OPEN + ',@down,@down,~2')
assert c[head + 3].lstrip('|').startswith('>Entry 12') and 'Entry 12' in c[top]

# A click on a header sorts by it; again turns it round.
c = run('sort', OPEN + ',C%d:%d,~2' % (title_x, head))
assert 'Entry 01' in c[head + 1] and 'Ridge trail' in c[head + 14]
c = run('sort2', OPEN + ',C%d:%d,~1,C%d:%d,~2' % (title_x, head, title_x, head))
assert 'Ridge trail' in c[head + 1] and 'Entry 01' in c[head + 14]

# Find reads the notes as well as the titles.
c = run('find', OPEN + ',M4,+f,M0,+ridge,~2')
assert 'ENTRIES 1 of 14' in c[top] and 'Ridge trail' in c[head + 1]
c = run('findnote', OPEN + ',M4,+f,M0,+eastern_path,~2')
assert 'ENTRIES 14 of 14' in c[top]                    # the fixture's entries share one note
c = run('findnone', OPEN + ',M4,+f,M0,+zzz,~2')
assert 'ENTRIES 0 of 14' in c[top] and has(c, 'Nothing matches Find.')
c = run('find2', OPEN + ',M4,+f,M0,+entry_07,~2')
assert 'ENTRIES 1 of 14' in c[top] and 'Entry 07' in c[head + 1]
c = run('findesc', OPEN + ',M4,+f,M0,+entry_07,@esc,~2')
assert 'ENTRIES 14 of 14' in c[top]

# The right button on an entry: its menu; Copy title says what it copied.
c = run('menu', OPEN + ',R%d:%d,~2' % (title_x + 3, head + 4))
assert all(has(c, t) for t in ('Edit the note...', 'Show on MAP', 'Copy title', 'Copy position'))
y = row_of(c, 'Copy title')
c = run('copy', OPEN + ',R%d:%d,~2,C%d:%d,~2' % (title_x + 3, head + 4, c[y].index('Copy title'), y))
assert 'Copied Entry 11' in c[44]

# The list goes to the card as text, a note an entry.
c = run('save', OPEN + ',M4,+e,M0,~2')
assert 'Saved 14 entries to journal-' in c[44]
text = sorted((work / 'build' / 'lssim_journal').glob('journal-*.txt'))[-1].read_text(encoding='utf-8')
assert text.startswith('== Ridge trail / north overlook\n') and text.count('\n== ') == 13
assert 'Mesh reception returned near the overlook.' in text and '910.5250 MHz' in text

# The handheld's pages: V is the live attachments and Esc comes back; Ctrl+L is its layout.
c = run('sensors', OPEN + ',+v,~3')
assert has(c, 'LIVE ATTACHMENTS') and ' Journal  View  Entry' not in c[1]
c = run('sensors2', OPEN + ',+v,~2,@esc,~2')
assert ' Journal  View  Entry' in c[1]
c = run('classic', OPEN + ',M4,+l,M0,~3')
assert ' Journal  View  Entry' not in c[1] and has(c, 'RECORD')
c = run('classic2', OPEN + ',M4,+l,+l,M0,~3')
assert ' Journal  View  Entry' in c[1]

# Record from the toolbar is the handheld's own request.
c = run('record', OPEN + ',C%d:2,~3' % c[2].index('Record'))
assert has(c, 'Queued; SD result appears below') or has(c, 'Record ON') or has(c, 'Request not accepted')

# A narrower window puts the entry under the table.
c = run('narrow', OPEN, grid='100x34')
assert has(c, 'ENTRIES 14 of 14') and has(c, 'Mesh reception returned')
print('JOURNAL desk: ok')
