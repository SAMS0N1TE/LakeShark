"""P25 in a desk window, driven as a PC drives it: the talkgroup table between
the handheld's view bar and its tuning deck, the call beside it, a header
clicked to sort, Hold and Lock out from the toolbar and the right button,
Find, the table saved as CSV, and the receiver view and the frequency
keyboard still a key away. Reads the cells lssim prints and the file the
click leaves on the card."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
work = sim.parent / 'p25-desk-ui'
work.mkdir(exist_ok=True)


def run(name, script, grid='160x48', **env):
    e = dict(os.environ, LSSIM_GRID=grid, LSSIM_P25_TGS='1', **env)
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


def tgs(cells, head, x, n=7):
    return [cells[head + 1 + i][x - 3:x + 2].strip(' .:>') for i in range(n)]


OPEN = '!p25,^,~10'

# The window: the handheld's views on top and its deck below; between them
# menus, the follow keys, the table, the call and the receiver's health.
c = run('open', OPEN)
assert '[ VFO ]' in c[1] and '[ CALLS ]' in c[1]
assert ' Receiver  View  Talkgroup' in c[2] and '851.0125 MHz' in c[2]
assert '[Follow ' in c[3] and '[Hold 1041]' in c[3] and '[Lock out 1041]' in c[3] and 'Find [' in c[3]
top = row_of(c, 'TALKGROUPS 7')
assert 'NOW PLAYING' in c[top] and 'voice decoded' in c[top]
head = top + 1
assert all(t in c[head] for t in ('TG', 'NAME', 'HITS', 'LAST', '16 MIN', 'CHANNEL', 'NAC', 'SEEN IN'))
assert '1041' in c[head + 1] and 'FIRE DISP' in c[head + 1] and 'now' in c[head + 1]      # on air, most recent first
assert 'E' in c[row_of(c, 'EMS NORTH')][:6]                                                 # heard encrypted
assert has(c, 'ON AIR') and has(c, 'UNIT 220158') and has(c, 'RECEIVER HEALTH')
assert 'TUNE [ STEP' in c[43] and 'VOLUME' in c[44]
assert '7 talkgroups' in c[42]
tg_x, hits_x, name_x = c[head].index('TG'), c[head].index('HITS'), c[head].index('NAME')
assert tgs(c, head, tg_x) == ['1041', '2104', '2051', '3301', '4005', '2110', '5120']

# A click on a header sorts by it; again turns it round.
c = run('sort', OPEN + ',C%d:%d,~2' % (hits_x, head))
assert tgs(c, head, tg_x)[:3] == ['1041', '2104', '2051']                                   # busiest first
c = run('sort2', OPEN + ',C%d:%d,~1,C%d:%d,~2' % (hits_x, head, hits_x, head))
assert tgs(c, head, tg_x)[-1] == '1041'
c = run('sorttg', OPEN + ',C%d:%d,~2' % (tg_x, head))
assert tgs(c, head, tg_x) == ['1041', '2051', '2104', '2110', '3301', '4005', '5120']

# A row chosen, then Hold on the toolbar: it is held, said in the row, the keys and the status.
row = row_of(c := run('again', OPEN), 'PD CENTRAL')
c = run('hold', OPEN + ',C%d:%d,~1,C%d:3,~2' % (name_x, row, c[3].index('Hold')))
assert '[Release 2104]' in c[3] and 'holding 2104' in c[42] and 'H' in c[row_of(c, 'PD CENTRAL')][:6]
# H on the keyboard is the same key, and lets it go again.
c = run('holdkey', OPEN + ',C%d:%d,~1,+h,~1,+h,~2' % (name_x, row))
assert '[Hold 2104]' in c[3] and 'holding' not in c[42]

# The right button on a talkgroup: hold, lock out, the allow list, copy.
c = run('menu', OPEN + ',R%d:%d,~2' % (name_x, row))
assert all(has(c, t) for t in ('Hold: follow only this one', 'Lock out', 'Add to the allow list', 'Copy talkgroup'))
y = row_of(c, 'Lock out', head + 1)
x = c[y].index('Lock out')
c = run('lock', OPEN + ',R%d:%d,~2,C%d:%d,~2' % (name_x, row, x, y))
assert '2104 locked out' in c[42] and '1 locked out' in c[42] and '[Unlock 2104]' in c[3]
assert 'L' in c[row_of(c, 'PD CENTRAL')][:6]
c = run('allow', OPEN + ',C%d:%d,~1,+a,~2' % (name_x, row))
assert '2104 added to the allow list' in c[42] and 'A' in c[row_of(c, 'PD CENTRAL')][:6]

# Find: a number or part of a name.
c = run('find', OPEN + ',M4,+f,M0,+pd,~2')
assert 'TALKGROUPS 2' in c[top] and tgs(c, head, tg_x, 2) == ['2104', '2110']
c = run('findesc', OPEN + ',M4,+f,M0,+pd,@esc,~2')
assert 'TALKGROUPS 7' in c[top]

# The table goes to the card as CSV.
c = run('save', OPEN + ',M4,+e,M0,~2')
assert 'Saved 7 talkgroups to talkgroups-' in c[42]
csv = sorted((work / 'build' / 'lssim_p25').glob('talkgroups-*.csv'))[-1].read_text(encoding='utf-8').splitlines()
assert csv[0].startswith('talkgroup,name,hits') and len(csv) == 8
assert csv[1].startswith('1041,FIRE DISP') and ',851.2625,293,lcw,' in csv[1]

# The handheld is still there: the receiver view by Ctrl+L and back, the
# frequency keyboard by T and Esc, the other views by their digits.
c = run('classic', OPEN + ',M4,+l,M0,~3')
assert 'P25 / RECEIVER' in c[2] and has(c, 'ACTIVITY')
c = run('classic2', OPEN + ',M4,+l,+l,M0,~3')
assert ' Receiver  View  Talkgroup' in c[2]
c = run('keypad', OPEN + ',+t,~3')
assert 'ENTER FREQUENCY' in c[2] and 'FREQUENCY [' in c[43] and not has(c, 'TALKGROUPS 7')
c = run('keypad2', OPEN + ',+t,~2,@esc,~2')
assert ' Receiver  View  Talkgroup' in c[2]
# Shift+L is still the handheld's "load a profile"; plain L locked out above.
c = run('profile', OPEN + ',+L,~3')
assert has(c, 'PROFILE') and '1 locked out' not in c[42]
c = run('views', OPEN + ',+3,~3,+1,~3')
assert ' Receiver  View  Talkgroup' in c[2]

# A narrower window drops the side column and keeps the table.
c = run('narrow', OPEN, grid='100x34')
assert has(c, 'TALKGROUPS 7') and has(c, 'PD CENTRAL')
print('P25 desk: ok')
