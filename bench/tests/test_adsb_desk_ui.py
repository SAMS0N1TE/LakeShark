"""ADS-B in a desk window, driven as a PC drives it: the aircraft table with
the emergency on top and in the frame, a header clicked to sort, Find, the
right button on a row, the table saved as CSV, and the handheld's own pages
(an aircraft's page, MAP ONLY) still a key away. Reads the cells lssim
prints and the file the click leaves on the card."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
work = sim.parent / 'adsb-desk-ui'
work.mkdir(exist_ok=True)


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


def row_of(cells, text, after=0):
    return next(y for y in sorted(cells) if y >= after and text in cells[y])


def has(cells, text):
    return any(text in line for line in cells.values())


OPEN = '!adsb,^,~10'

# The window: menus, a toolbar, filters, the table, the map, this aircraft, traffic.
c = run('open', OPEN)
assert ' Receiver  View  Aircraft' in c[1] and '7 aircraft' in c[1]
assert '[Radio]' in c[2] and '[Follow ' in c[2] and '[Save CSV]' in c[2]
assert 'Find [' in c[3] and '[Emergencies first]' in c[3] and 'as tracked' in c[3]
top = row_of(c, 'AIRCRAFT 7 of 7')
assert '! N55EM EMERGENCY' in c[top] and 'MINI MAP' in c[top]
head = top + 1
assert all(t in c[head] for t in ('CALLSIGN', 'ICAO', 'ALT ft', 'V/S', 'KT', 'HDG', 'NM', 'BRG', 'SQWK', 'SEEN'))
assert '! N55EM' in c[head + 1] and '7700' in c[head + 1]              # the emergency, first
assert 'UAL442' in c[head + 2] and c[head + 2].lstrip('|').startswith('>')   # the tracker's selection
assert has(c, 'THIS ONE') and has(c, 'A3 large') and has(c, '34000 ft') and has(c, 'HISTORY')
assert '1 emergency' in c[44]
alt_x, call_x = c[head].index('ALT ft'), c[head].index('CALLSIGN')

# A click on a header sorts by it, again turns it round, a third time is the tracker's order.
c = run('sort', OPEN + ',C%d:%d,~2' % (alt_x, head))
assert 'by ALT ft' in c[3]
order = [c[head + 1 + i][call_x:call_x + 8].strip() for i in range(7)]
assert order[0] == 'N55EM' and order[1] == 'LIFE1' and order[-1].startswith('UAL442'), order
c = run('sort2', OPEN + ',C%d:%d,~1,C%d:%d,~2' % (alt_x, head, alt_x, head))
assert 'turned round' in c[3] and 'N55EM' in c[head + 1] and 'UAL442' in c[head + 2]
c = run('sort3', OPEN + ',C%d:%d,~1,C%d:%d,~1,C%d:%d,~2' % (alt_x, head, alt_x, head, alt_x, head))
assert 'as tracked' in c[3]
# With the emergency unpinned it sorts where its column puts it.
pin = c[3].index('Emergencies first')
c = run('unpin', OPEN + ',C%d:3,~1,C%d:%d,~2' % (pin, alt_x, head))
assert 'LIFE1' in c[head + 1] and 'N55EM' in c[head + 3]

# The arrows move the tracker's selection; the details follow.
c = run('down', OPEN + ',@down,~2')
assert c[head + 3].lstrip('|').startswith('>') and has(c, '5500 ft')

# Find: a squawk, an address or a callsign.
c = run('find', OPEN + ',M4,+f,M0,+7700,~2')
assert 'AIRCRAFT 1 of 7' in c[top] and 'N55EM' in c[head + 1] and not has(c, 'FDX903   ')
c = run('findesc', OPEN + ',M4,+f,M0,+7700,@esc,~2')
assert 'AIRCRAFT 7 of 7' in c[top]

# The right button on a row: its menu; Copy ICAO says what it copied.
c = run('menu', OPEN + ',R%d:%d,~2' % (call_x + 2, head + 3))
assert all(has(c, t) for t in ('Its own page', 'Show on MAP', 'Copy callsign', 'Copy ICAO address', 'Copy the row as CSV'))
y = row_of(c, 'Copy ICAO address')
c = run('copy', OPEN + ',R%d:%d,~2,C%d:%d,~2' % (call_x + 2, head + 3, c[y].index('Copy ICAO'), y))
assert 'Copied A4C5D6' in c[44]

# The table goes to the card as CSV, in the order shown.
c = run('save', OPEN + ',M4,+e,M0,~2')
assert 'Saved 7 aircraft to aircraft-' in c[44]
csv = sorted((work / 'build' / 'lssim_adsb').glob('aircraft-*.csv'))[-1].read_text(encoding='utf-8').splitlines()
assert csv[0].startswith('icao,callsign,altitude_ft') and len(csv) == 8
assert csv[1].startswith('A77000,N55EM,3500,-900,120,140,') and ',7700,EMERGENCY,' in csv[1]

# Enter is the aircraft's own page, Esc comes back; V is MAP ONLY and back; Ctrl+L the handheld layout.
c = run('page', OPEN + ',@enter,~3')
assert 'A1B2C3' in c[2] and 'CALLSIGN' in c[3] and 'RX CRC' in c[row_of(c, 'RX CRC')]
c = run('pageback', OPEN + ',@enter,~2,@esc,~2')
assert ' Receiver  View  Aircraft' in c[1]
c = run('maponly', OPEN + ',+v,~3')
assert 'MAP ONLY' not in c[1] and '[ LIST ]' in c[1]
c = run('maponly2', OPEN + ',+v,~2,+v,~2')
assert ' Receiver  View  Aircraft' in c[1]
c = run('classic', OPEN + ',M4,+l,M0,~3')
assert 'AIRCRAFT (* stale)' in c[2]
c = run('classic2', OPEN + ',M4,+l,+l,M0,~3')
assert ' Receiver  View  Aircraft' in c[1]

# A narrower window drops the map and puts the details in a strip under the table.
c = run('narrow', OPEN, grid='100x34')
assert 'MINI MAP' not in c[4] and has(c, 'THIS ONE') and has(c, 'from here')
print('ADS-B desk: ok')
