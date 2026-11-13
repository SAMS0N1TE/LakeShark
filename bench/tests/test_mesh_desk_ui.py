"""MESH in a desk window, driven as a PC drives it: the chat list and its
unread counts, a conversation opened by a click, a line typed and sent, the
node table sorted by a header, the right button on a node and on a message,
Find, copy and paste, the chat saved to the card, and the handheld layout
and a node's INSPECT page still there. Reads the cells lssim prints and what
the mesh stub says it was asked to send."""
from pathlib import Path
import os
import subprocess
import sys

sim = Path(sys.argv[1]).resolve()
work = sim.parent / 'mesh-desk-ui'
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
    return cells, result.stderr


def row_of(cells, text, after=0):
    return next(y for y in sorted(cells) if y >= after and text in cells[y])


def has(cells, text):
    return any(text in line for line in cells.values())


OPEN = '!mesh,^,~10'

# The window: a menu bar, a toolbar, the chat list, the conversation with its
# message line, what is on the channel, a status line.
c, _ = run('open', OPEN, LSSIM_MESH_LAST='dm')
assert ' Mesh  Chat  View  Node' in c[1] and 'LISTENING' in c[1]
assert '[Chats 1]' in c[2] and '[Nodes 6]' in c[2] and '[Arm]' in c[2]
top = row_of(c, 'CHATS')
assert '1 unread' in c[top] and '# public' in c[top] and 'ON THIS CHANNEL' in c[top]
kb = row_of(c, '@ KB1QWE')
assert c[kb].split('|')[1].rstrip().endswith(' 1')                      # the DM that has not been read
assert '^ NORTHFIELD' in c[kb + 1] and has(c, '~ WEIR TEMP')
send = row_of(c, 'To # public')
assert '[Send]' in c[send] and 'testing the new firmware now' in c[send - 2] and 'sending' in c[send - 2]
assert 'relayed x3' in c[row_of(c, 'copy, hearing you at -74')]
assert '1 unread' in c[send + 2] and 'Disarmed' in c[send + 2]
kb_x = c[kb].index('KB1QWE')

# A click on a chat opens it: the new line, the node beside it, nothing unread.
c, _ = run('dm', OPEN + ',C%d:%d,~2' % (kb_x, kb), LSSIM_MESH_LAST='dm')
assert '@ KB1QWE' in c[top] and '2 messages' in c[top] and 'NODE' in c[top]
assert ' new ' in c[row_of(c, 'are you on the lake today') - 1]
assert 'delivered' in c[row_of(c, 'bringing the handheld')]
assert '0 unread' in c[send + 2] and '17BC5E90A3D46F82' in c[top + 2] and '775 m' in c[row_of(c, 'range ')]

# Disarmed, a typed line stays a draft; armed it goes where the chat is.
c, err = run('draft', OPEN + ',+hello_all,@enter,~2')
assert 'hello all' in c[send] and 'Disarmed' in c[send + 2] and 'send_on' not in err
c, err = run('send', OPEN + ',C%d:2,~1,+hello_all,@enter,~2' % c[2].index('Arm'))
assert 'mesh-stub send_on 0 hello all' in err and 'hello all' not in c[send] and 'Message queued' in c[send + 2]
c, err = run('slash', OPEN + ',+/arm,@enter,~2')
assert 'ARMED' in c[1] and '[Disarm]' in c[2]

# Nodes: a table, sorted by a click on a header, the right button on a row.
nodes = c[2].index('Nodes')
c, _ = run('nodes', OPEN + ',C%d:2,~2' % nodes)
head = row_of(c, ' NAME ')
assert all(t in c[head] for t in ('NAME', 'ID', 'ROLE', 'HEARD', 'RSSI', 'SNR', 'RANGE', 'ADVERTS'))
assert 'NORTHFIELD' in c[head + 1] and 'KB1QWE' in c[head + 2]          # the most recently heard first
name_x, rssi_x = c[head].index('NAME'), c[head].index('RSSI')
c, _ = run('sort', OPEN + ',C%d:2,~2,C%d:%d,~2' % (nodes, rssi_x, head))
assert 'NORTHFIELD' in c[head + 1] and 'TILTON ROOM' in c[head + 2] and 'KB1QWE' in c[head + 3]
assert '9E7A410B' in c[head + 6]                                        # the weakest last
c, _ = run('sort2', OPEN + ',C%d:2,~2,C%d:%d,~1,C%d:%d,~2' % (nodes, rssi_x, head, rssi_x, head))
assert '9E7A410B' in c[head + 1] and 'NORTHFIELD' in c[head + 6]
c, _ = run('nodemenu', OPEN + ',C%d:2,~2,R%d:%d,~2' % (nodes, name_x + 2, head + 2))
assert all(has(c, t) for t in ('Message', 'Show on MAP', 'Inspect: ping, trace, telemetry', 'Copy id', 'Copy position'))
map_y = row_of(c, 'Show on MAP')
map_x = c[map_y].index('Show on MAP')
# Message, the row above Show on MAP: the chat with that node opens.
c, _ = run('nodedm', OPEN + ',C%d:2,~2,R%d:%d,~2,C%d:%d,~2' % (nodes, name_x + 2, head + 2, map_x, map_y - 1))
assert '@ KB1QWE' in c[top] and 'To @ KB1QWE' in c[send]
# Enter on a row is the keyboard's way to the same place.
c, _ = run('nodeenter', OPEN + ',C%d:2,~2,@down,@enter,~2' % nodes)
assert 'To @ KB1QWE' in c[send]

# The right button on a message, copy, and paste into the line of another chat.
c, _ = run('again', OPEN)
msg = row_of(c, 'anyone near the dam')
c, _ = run('msgmenu', OPEN + ',R60:%d,~2' % msg)
assert all(has(c, t) for t in ('Copy text', 'Copy sender', 'Quote in the reply', 'Message the sender'))
c, _ = run('paste', OPEN + ',C60:%d,~1,M4,+c,M0,~1,C%d:%d,~2,M4,+v,M0,~2' % (msg, kb_x, kb))
assert 'To @ KB1QWE [anyone near the dam this afternoon' in c[send]

# Find narrows the chat; Esc gives it back.
c, _ = run('find', OPEN + ',M4,+f,M0,+DAM,~2')
assert 'Find [DAM' in c[top + 1] and '1 of 6 match' in c[top] and 'anyone near the dam' in c[send - 2]
assert not has(c, 'net at 1900')
c, _ = run('findesc', OPEN + ',M4,+f,M0,+DAM,@esc,~2')
assert '6 messages' in c[top] and has(c, 'net at 1900')

# A long message wraps and loses nothing; the wheel reads back.
c, _ = run('long', OPEN, grid='100x34', LSSIM_MESH_LAST='long')
assert has(c, 'FINALTAIL')
c, _ = run('fill', OPEN + ',W60:12:-2,~2', grid='100x30', LSSIM_MESH_FILL='1')
assert has(c, 'more below: End') and has(c, 'earlier message 01')

# The chat goes to the card as a text file.
c, _ = run('save', OPEN + ',M4,+e,M0,~2')
assert 'Saved 6 messages to public-' in c[send + 2]
saved = sorted((work / 'build' / 'lssim_mesh').glob('public-*.txt'))[-1].read_text(encoding='utf-8').splitlines()
assert saved[0].startswith('MESH channel public') and len(saved) == 7
assert saved[2].split('\t')[2:] == ['SHARK', 'copy, hearing you at -74', 'relayed']

# The handheld layout and a node's INSPECT page are still there; Ctrl+L and Esc come back.
c, _ = run('classic', OPEN + ',M4,+l,M0,~3')
assert 'PUBLIC CHANNEL' in c[2] and 'HEARD' in c[2] and 'Ctrl+L brings the desktop one back' in c[44]
c, _ = run('back', OPEN + ',M4,+l,+l,M0,~3')
assert ' Mesh  Chat  View  Node' in c[1]
c, _ = run('menu', OPEN + ',R%d:%d,~2' % (kb_x, kb + 1))
insp = row_of(c, 'Inspect: ping')
insp_x = c[insp].index('Inspect')
c, _ = run('inspect', OPEN + ',R%d:%d,~2,C%d:%d,~3' % (kb_x, kb + 1, insp_x, insp))
assert 'INSPECT' in c[2] and 'NORTHFIELD' in c[3] and has(c, 'TRACE route')
c, _ = run('inspectback', OPEN + ',R%d:%d,~2,C%d:%d,~3,@esc,@esc,~2' % (kb_x, kb + 1, insp_x, insp))
assert ' Mesh  Chat  View  Node' in c[1]

# A narrower window drops the detail pane and keeps the rest.
c, _ = run('narrow', OPEN, grid='100x34')
assert 'CHATS' in c[3] and 'ON THIS CHANNEL' not in c[3] and '[Send]' in c[28]
print('MESH desk: ok')
