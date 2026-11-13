"""FREQS in a desk window, driven as a PC drives it: the mouse (click, Ctrl and
Shift click, the right button, a drag onto a bank, the wheel), the keyboard
(Ctrl+F, Shift+arrows, Del, Ctrl+Z, F2), and the window at three sizes. Reads
the cells lssim prints and the bank files the clicks leave on the card."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
banks = Path(sys.argv[2]).resolve()          # bench/fixtures/freqs_test: cordoba.csv and mine.csv
work = sim.parent / 'freqs-desk-ui'
work.mkdir(exist_ok=True)
card = work / 'build' / 'lssim_sdcard' / 'freqs'


def run(name, script, grid='160x48'):
    env = dict(os.environ, LSSIM_GRID=grid, LSSIM_FREQS_FROM=str(banks))
    result = subprocess.run([str(sim), 'desk', '-x', script, '-d', '-o', str(work / (name + '.bmp'))],
                            capture_output=True, text=True, check=True, cwd=work, env=env)
    (work / (name + '.txt')).write_text(result.stdout, encoding='utf-8')
    cells = {}
    for line in result.stdout.splitlines():
        if line[:4].strip().isdigit():
            cells[int(line[:4])] = line[4:]
    return cells


def row_of(cells, text, after=0):
    return next(y for y in sorted(cells) if y >= after and text in cells[y])


def col_of(cells, y, text):
    return cells[y].index(text)


def lines(name):
    return (card / name).read_text(encoding='utf-8').splitlines()


OPEN = '!freqs,^,~40'

# The window: a menu bar, the toolbar, filters, the ruler, three panes, a status line.
c = run('open', OPEN)
assert ' File  Edit  View  Tune' in c[1]
assert '[New]' in c[2] and '[Delete]' in c[2] and 'Find [' in c[3] and '[Band ALL]' in c[3]
assert 'MHz' in c[4]                                   # the ruler's scale
head = row_of(c, ' NAME ')
assert all(t in c[head] for t in ('MHz', 'NAME', 'MODE', 'GROUP', 'DIST BANK'))
assert 'BANKS' in c[head - 1] and '21 of 21' in c[head - 1] and 'THIS ONE' in c[head - 1]
all_y, cordoba_y, mine_y = (row_of(c, t) for t in ('> ALL BANKS', '  CORDOBA  ', '  MINE  '))
assert ' 21|' in c[all_y] and ' 19|' in c[cordoba_y] and '  2|' in c[mine_y]
assert 'GROUPS' in c[row_of(c, 'GROUPS')] and 'AVIATION' in c[row_of(c, 'GROUPS') + 1]
assert 'CORDOBA TWR' in c[head + 1] and 'CORDOBA TWR' in c[head]       # the list, and THIS ONE beside it
assert '21 of 21' in c[44]
mhz_x = col_of(c, head, 'MHz')
name_x = col_of(c, head, 'NAME')
bank_x = col_of(c, cordoba_y, 'CORDOBA')

# A click on a header sorts by it, again turns it round, a third time is the file's order.
c = run('sort1', OPEN + f',C{mhz_x}:{head}')
assert '0.700' in c[head + 1] and 'LV3' in c[head + 1]
c = run('sort2', OPEN + f',C{mhz_x}:{head},C{mhz_x}:{head}')
assert '1090.000' in c[head + 1]
c = run('sort3', OPEN + f',C{mhz_x}:{head},C{mhz_x}:{head},C{mhz_x}:{head}')
assert 'CORDOBA TWR' in c[head + 1]

# A bank in the tree opens it; Ctrl+F filters as it is typed; Esc clears.
PICK = OPEN + f',C{bank_x}:{cordoba_y},~10'
c = run('bank', PICK)
assert '19 of 19' in c[head - 1] and 'FREQS / CORDOBA' in c[1]
assert 'DIST BANK' not in c[head] and 'TONE' in c[head]                           # one bank: no bank column
c = run('find', PICK + ',M4,+f,M0,+repet')
assert 'Find [repet' in c[3] and '2 of 19' in c[head - 1] and 'REPETIDORA' in c[head + 1]
c = run('find-esc', PICK + ',M4,+f,M0,+repet,@esc')
assert '19 of 19' in c[head - 1] and 'Find [ ' in c[3]

# Shift+arrows select a run; Del asks, deletes from the file, and Ctrl+Z puts them back.
SEL = PICK + ',@down,M1,@down,@down,M0'
c = run('select', SEL)
assert '3 selected' in c[44] and 'SELECTION' in c[head - 1] and '3 entries' in c[head]
c = run('delete-ask', SEL + ',@del')
assert any('Delete 3 entries' in c[y] for y in c)
c = run('delete', SEL + ',@del,@enter,~10')
assert '16 of 16' in c[head - 1] and 'Deleted 3' in c[44] and 'undo 3' in c[44]
kept = lines('cordoba.csv')
assert not any('CORDOBA GND' in s or 'CORDOBA APP' in s or 'CORDOBA ATIS' in s for s in kept)
assert any('CORDOBA TWR' in s for s in kept) and sum(s.startswith('#') for s in kept) == 3
c = run('undo', SEL + ',@del,@enter,~10,M4,+z,M0,~10')
assert '19 of 19' in c[head - 1] and 'Put back 3' in c[44]
assert sum('CORDOBA GND' in s for s in lines('cordoba.csv')) == 1

# Ctrl+click picks rows one by one; the right button's menu acts on them.
CTRL = PICK + f',M4,C{name_x}:{head + 1},C{name_x}:{head + 3},C{name_x}:{head + 5},M0'
c = run('ctrl', CTRL)
assert '3 selected' in c[44]
c = run('context', CTRL + f',R{name_x}:{head + 3}')
menu = row_of(c, '3 SELECTED')
assert 'Copy to bank...' in c[row_of(c, 'Copy to bank...', menu)] and 'Delete' in c[row_of(c, 'Delete', menu + 1)]
# Outside the selection the right button takes that row alone.
c = run('context-one', CTRL + f',R{name_x}:{head + 8}')
assert not any('3 SELECTED' in c[y] for y in c) and any('Move to bank...' in c[y] for y in c)
# The header's menu sorts and shows columns.
c = run('context-head', PICK + f',R{name_x}:{head}')
assert any('SORT BY' in c[y] for y in c) and any('COLUMNS' in c[y] for y in c)

# A row carried onto a bank moves it there.
c = run('drag', PICK + f',D{name_x}:{head + 3}>{bank_x}:{mine_y},~10')
assert 'Moved 1 to mine' in c[44] and '18 of 18' in c[head - 1]
assert sum('CORDOBA APP' in s for s in lines('mine.csv')) == 1
assert not any('CORDOBA APP' in s for s in lines('cordoba.csv'))
# Dragged inside the list it sweeps out a selection instead.
c = run('sweep', PICK + f',D{name_x}:{head + 2}>{name_x}:{head + 6}')
assert '5 selected' in c[44]

# F2 edits in place: a field is checked, the file gets only what changed.
c = run('edit', PICK + ',@f2')
assert 'EDIT' in c[head - 1] and 'Name   [CORDOBA TWR' in c[head + 1] and 'MHz    [118.3' in c[head]
c = run('edit-bad', PICK + ',@f2,@up,+x,@enter')
assert 'MHz: a number from 0.009 to 6000' in c[44] and 'EDIT' in c[head - 1]
c = run('edit-ok', PICK + ',@f2,+_NORTE,@enter,~10')
assert 'Changed' in c[44] and 'CORDOBA TWR NORTE' in c[head + 1]
assert any(s.startswith('118.3,AM,,CORDOBA TWR NORTE,AVIATION,SACO,-31.3236,-64.2080,40') for s in lines('cordoba.csv'))

# Ins adds to the bank in view; a new bank from File; the star is a click on its column.
c = run('new', PICK + ',@ins,+145.5,@tab,+CALLING,@enter,~10')
assert 'Saved 145.500 CALLING to cordoba' in c[44] and '20 of 20' in c[head - 1]
c = run('new-bank', OPEN + ',C3:1,@down,@enter,+Field_Day,@enter,~10')
assert 'FIELD-DAY' in c[row_of(c, 'FIELD-DAY')] and (card / 'field-day.csv').exists()
star_x = col_of(c, head, '*')
c = run('star', PICK + f',C{star_x}:{head + 2},~10')
assert 'Starred CORDOBA GND' in c[44] and c[head + 2][star_x] == '*'
assert any('CORDOBA GND' in s and ',fav SACO,' in s for s in lines('cordoba.csv'))

# Copy two rows, open MINE, paste; Alt+E is the Edit menu; a double click tunes (FM's window opens).
c = run('paste', PICK + f',@down,M1,@down,M4,+c,M0,C{bank_x}:{mine_y},~10,M4,+v,M0,~10')
assert 'Pasted 2 into mine' in c[44] and '4 of 4' in c[head - 1]
assert sum('CORDOBA GND' in s or 'CORDOBA APP' in s for s in lines('mine.csv')) == 2
assert sum('CORDOBA GND' in s for s in lines('cordoba.csv')) == 1
c = run('alt-menu', OPEN + ',M2,+e,M0')
assert any('Duplicate' in c[y] and 'Ctrl+D' in c[y] for y in c) and any('Undo delete' in c[y] for y in c)
c = run('double', PICK + f',C{name_x}:{head + 7},C{name_x}:{head + 7},~5')
assert any('] ~ FM ' in c[y] for y in c)
c = run('single', PICK + f',C{name_x}:{head + 7},=700,C{name_x}:{head + 7},~5')
assert not any('] ~ FM ' in c[y] for y in c)

# The wheel carries the list, not the cursor.
c = run('wheel', OPEN + f',W{name_x}:{head + 5}:2', '160x30')
head = row_of(c, ' NAME ')
assert 'CORDOBA TWR' not in c[head + 1] and 'CORDOBA TWR' in c[head] and 'CORDOBA' in c[head + 1]

# Smaller windows: the detail pane goes under the list, then away; nothing is cut in half.
c = run('window', '!freqs,~40')
top = row_of(c, 'File  Edit  View  Tune')
assert 'FREQS' in c[top - 1] and '[-]' in c[top - 1]      # the window's title row
assert any('THIS ONE' in c[y] for y in c) and any('BANKS' in c[y] for y in c)
c = run('small', OPEN, '100x34')
assert ' File  Edit  View  Tune' in c[1] and any('THIS ONE' in c[y] for y in c)
head = row_of(c, ' NAME ')
assert 'MODE' in c[head] and 'NOTES' not in c[head] and 'CORDOBA TWR' in c[head + 1]
c = run('tiny', OPEN + ',@f2', '80x24')
assert ' File  Edit  View  Tune' in c[0] and any('Name   [CORDOBA TWR' in c[y] for y in c)
print('FREQS desk: layout, sort, find, select, delete/undo, menus, drag, edit, new, star, paste, tune, wheel, sizes PASS')
