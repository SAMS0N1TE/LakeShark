# SPDX-License-Identifier: GPL-3.0-or-later
import ctypes as c
from pathlib import Path
import numpy as np
import json, time, sys, subprocess, tempfile, hashlib

root = Path(__file__).resolve().parent
repo = root.parents[1]
build = tempfile.TemporaryDirectory(prefix="ls_recorded_lte_")
library = Path(build.name)/"lte_recorded.dll"
src = repo/"components/lakeshark/apps/cell"
subprocess.run(["gcc", "-O2", "-shared", "-fPIC",
                "-o", str(library), *[str(src/(n+".c")) for n in ("lte_sync", "lte_resample", "lte_mib")], "-lm"], check=True)
dll = c.CDLL(str(library))
for name in ('lte_sync', 'lte_resample', 'lte_mib'):
    getattr(dll, name+'_workspace_size').restype = c.c_size_t
dll.lte_resample_count.argtypes = [c.c_size_t, c.c_uint32]
dll.lte_resample_count.restype = c.c_size_t
dll.lte_resample_u8.argtypes = [c.c_void_p, c.c_size_t, c.c_uint32, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, c.c_void_p]
dll.lte_resample_u8.restype = c.c_size_t
class Sync(c.Structure):
    _fields_ = [('pci', c.c_int), ('hits', c.c_int), ('pairs', c.c_int), ('cfo', c.c_int), ('pss', c.c_float), ('sss', c.c_float)]
class Mib(c.Structure):
    _fields_ = [(name, c.c_int) for name in ('n_rb', 'ports', 'phich_duration', 'phich_resource', 'sfn', 'frames', 'first_frame')] + [('payload', c.c_uint32)]
dll.lte_sync_find.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p, c.POINTER(Sync), c.c_void_p, c.c_void_p]
dll.lte_sync_find.restype = c.c_bool
dll.lte_sync_frame_start.argtypes = [c.c_void_p]
dll.lte_sync_frame_start.restype = c.c_int
dll.lte_mib_find.argtypes = [c.c_void_p, c.c_size_t, c.c_int, c.c_int, c.c_int, c.c_void_p, c.POINTER(Mib), c.c_void_p, c.c_void_p]
dll.lte_mib_find.restype = c.c_bool
dll.lte_mib_confirm.argtypes = dll.lte_mib_find.argtypes
dll.lte_mib_confirm.restype = c.c_bool
def decode(name, raw, rate):
    start = time.monotonic()
    size = dll.lte_resample_count(len(raw)//2, rate)
    iq = np.zeros(size*2, np.uint8)
    ws = c.create_string_buffer(dll.lte_resample_workspace_size())
    assert dll.lte_resample_u8(raw.ctypes.data, len(raw)//2, rate, iq.ctypes.data, size, ws, None, None) == size
    sws = c.create_string_buffer(dll.lte_sync_workspace_size())
    sync = Sync()
    found = dll.lte_sync_find(iq.ctypes.data, size, sws, c.byref(sync), None, None)
    frame = dll.lte_sync_frame_start(sws)
    mws = c.create_string_buffer(dll.lte_mib_workspace_size())
    mib = Mib()
    decoded = dll.lte_mib_find(iq.ctypes.data, size, sync.pci, frame, sync.cfo, mws, c.byref(mib), None, None) if found else False
    quick = Mib()
    confirmed = dll.lte_mib_confirm(iq.ctypes.data, size, sync.pci, frame, sync.cfo, mws, c.byref(quick), None, None) if found else False
    assert confirmed == decoded, 'Fast confirmation disagrees with exhaustive MIB result'
    if confirmed:
        assert quick.frames == 2
        for key, _ in Mib._fields_:
            if key != 'frames':
                assert getattr(quick,key) == getattr(mib,key), ('MIB confirmation mismatch',key)
    result = dict(name=name, sync=found, timing=frame, **{k: getattr(sync, k) for k, _ in sync._fields_}, mib=decoded, result={k: getattr(mib,k) for k,_ in mib._fields_}, seconds=round(time.monotonic()-start, 3))
    print(json.dumps(result), flush=True)
    return result
if __name__ == '__main__':
    try:
        for fixture in json.loads((root/'recordings.json').read_text()):
            data=(root/fixture['file']).read_bytes()
            assert len(data)==fixture['bytes'] and hashlib.sha256(data).hexdigest()==fixture['sha256']
            result=decode(fixture['file'],np.frombuffer(data,np.uint8),fixture['rate'])
            expected=fixture['expected']
            assert result['sync']==expected['sync']
            if expected['sync']:
                assert result['pci']==expected['pci'] and result['mib']
                for key in ('n_rb','ports','sfn','frames'):
                    assert result['result'][key]==expected[key],key
            else:
                assert not result['mib']
        print('PASS: two recorded MIB decodes; discontinuous recording rejected')
    finally:
        if hasattr(c,'windll'):
            import _ctypes
            _ctypes.FreeLibrary(dll._handle)
        build.cleanup()
