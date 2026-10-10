"""Host stack/call-graph gate for the production CartoCore TUI boundary."""
from pathlib import Path
import os,re,subprocess,sys
root=Path(__file__).resolve().parents[2]
build=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'bench/build').resolve()
roots={'ls_cells_query','ls_cells_pick','ls_cells_action','ls_cells_server','ls_cells_snapshot','ls_carto_map_draw','ls_carto_map_limits','ls_carto_map_leave',
       'ls_cartocore_draw','ls_cartocore_dismiss','ls_carto_map_prepare',
       'ls_carto_map_select','ls_carto_map_status','ls_carto_map_files','ls_carto_map_labels','ls_carto_map_fresh'}
frames={}
for filename in ('ls_cartocore.c','tui_core.c'):
    path=next(p for p in build.rglob(filename+'.su') if 'carto_stack_audit.dir' in str(p))
    for line in path.read_text().splitlines():
        location,size,kind=line.split('\t');frames[location.rsplit(':',1)[-1]]=(int(size),kind)
graph={};name=None;indirect=set()
for filename in ('ls_cartocore.c','tui_core.c'):
    path=next(p for p in build.rglob(filename+'.*.cgraph') if 'carto_stack_audit.dir' in str(p))
    text=path.read_text().split('Initial Symbol table:',1)[1].split('Removing unused symbols:',1)[0]
    for line in text.splitlines():
        m=re.match(r'^(\w+)/\d+ \(',line)
        if m:name=m[1]
        if 'Indirect call' in line:indirect.add(name)
        if line.startswith('  Calls:'):
            calls=re.findall(r'(\w+)/\d+',line)
            if calls or name not in graph:graph[name]=calls
# External leaves are deliberately closed: adding a dependency requires review.
leaves={'memcpy','strlen','strcpy','strcmp','heap_caps_free','_assert'}
seen=set();pending=list(roots)
while pending:
    name=pending.pop()
    if name in seen:continue
    seen.add(name)
    assert name not in indirect,('unreviewed indirect call',name)
    assert not re.search(r'(fopen|opendir|readdir|(?:^|_)stat$|rename|unlink|fread|fclose|lseek|nvs_|open_map|recover_maps|render_frame)',name),name
    if name in frames:
        size,kind=frames[name]
        assert size<=512 and kind=='static',(name,size,kind)
    else:
        assert name in leaves or name.startswith('__atomic_'),('unreviewed external or missing stack record',name)
    pending+=graph.get(name,[])
assert roots<=frames.keys()
for api in ('fopen','opendir','stat','rename','unlink','nvs'):
    env=dict(os.environ,LSSIM_CARTO_GUARD_PROBE=api)
    p=subprocess.run([str(build/'lssim.exe'),'map','-f','1'],cwd=root,env=env,capture_output=True,text=True)
    assert p.returncode==(87 if api=='nvs' else 86),(api,p.returncode,p.stderr)
print(f'PASS: {len(roots)} TUI roots / {len(seen)} reachable symbols; max frame {max(frames[n][0] for n in seen if n in frames)} B; no file/flash callees; 6 runtime negative probes rejected')
