"""Exercise production PNG task framing with threaded host logs; no hardware."""
from pathlib import Path
import base64
import importlib.util
import shutil
import subprocess
import sys
import types
import zlib

root = Path(__file__).resolve().parents[2]
out = root / 'bench/build/png-console-test'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/compact_ui.cpp').read_text()
task = source[source.index('struct TuiPngJob {'):source.index("/* 'tui rec' sender.")]
raw = (root / 'bench/fixtures/tui/tiles-list-portrait.png').read_bytes()
encoded = base64.b64encode(raw).decode()
code = r'''
#include <stdio.h>
#include <stdint.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdarg.h>
#define flockfile _lock_file
#define funlockfile _unlock_file
using TaskHandle_t=void *;
static void xTaskNotifyGive(void *) {}
static void vTaskSuspend(void *) {}
static const char data[]="ENCODED";
static size_t ls_tui_png_emit(const uint16_t *,int,int,bool,void (*emit)(const char *,void *),void *ctx,uint32_t *crc) {
    for(size_t i=0;i<sizeof(data)-1;i+=76) {
        char line[77]; snprintf(line,sizeof(line),"%.*s",(int)(sizeof(data)-1-i<76?sizeof(data)-1-i:76),data+i);
        emit(line,ctx); std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    *crc=CRC; return SIZE;
}
'''.replace('CRC', str(zlib.crc32(raw) & 0xffffffff)).replace('SIZE', str(len(raw))).replace('ENCODED', encoded)
code += task + r'''
static void logprintf(const char *fmt,...) { va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); }
int main() {
    std::atomic<bool> running{true};
    std::thread logs([&] { while(running) { logprintf("TASK log during transfer\n"); std::this_thread::sleep_for(std::chrono::microseconds(50)); } });
    TuiPngJob job{}; snprintf(job.name,sizeof(job.name),"host"); job.w=568; job.h=1232;
    tui_png_task(&job); running=false; logs.join();
}
'''
(out / 'test.cpp').write_text(code)
gcc = shutil.which('g++') or 'C:/mingw64/bin/g++.exe'
subprocess.run([gcc, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', str(out / 'test.cpp'), '-o', str(out / 'test.exe')], check=True)
result = subprocess.run([str(out / 'test.exe')], capture_output=True, check=True)
(out / 'transcript.txt').write_bytes(result.stdout)
text = result.stdout.decode()
framed = text[text.index('tui: png begin'):text.index('tui: png end')]
assert 'TASK log during transfer' in framed
assert all('TASK' not in line for line in framed.splitlines() if line.startswith('~'))
# Feed the real receiver in fragmented serial reads.
sys.modules.setdefault('serial', types.ModuleType('serial'))
spec = importlib.util.spec_from_file_location('console', root / 'tools/ls_console.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
class Serial:
    def __init__(self, data): self.data = data
    @property
    def in_waiting(self): return min(113, len(self.data))
    def read(self, n):
        chunk, self.data = self.data[:n], self.data[n:]
        return chunk
    def reset_input_buffer(self): pass
console = module.Console.__new__(module.Console)
console.serial = Serial(result.stdout)
console.wait_idle = lambda: None
console.send = lambda command: None
size, crc = console.capture_png(out / 'received.png', timeout=5)
assert size == len(raw) and (out / 'received.png').read_bytes() == raw
print('PASS: production PNG per-record locks, concurrent vprintf logs, fragmented host receive, size and CRC')
