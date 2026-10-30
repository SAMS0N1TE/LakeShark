"""The cells queue walk gives the worker back after 200 ms, so MAP is served.

Same production source and stubs as test_tiles_queue.py, with a clock that
moves 150 ms every time it is read: a step verifies at most a couple of
cells, the walk resumes where it stopped, and the place still completes.
"""
from pathlib import Path
import re
import shutil
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[1]
text = (here / 'test_tiles_queue.py').read_text()
text = text.replace("bench/build/tiles-queue", "bench/build/tiles-queue-budget")
clock = 'static int64_t esp_timer_get_time(void) { return 0; }'
assert clock in text
text = text.replace(clock, 'static int64_t clock_us;\n'
                    'static int64_t esp_timer_get_time(void) { return clock_us+=150000; }')
main = r'''int main(void) {
    (void)cells_step;(void)cells_catalog; /* in the shared source, not walked here */
    cw.initialized=true;cw.catalog=1;cw.queue_loaded=true;cw.progress_loaded=true;
    for(unsigned i=0;i<8;i++) cw.queued[i]=0xff; /* 64 queued cells, all on the card */
    cells_queue_step();
    assert(cw.queue_loaded && !records && cw.next_cell>0 && cw.next_cell<8);
    unsigned steps=1;
    while(cw.queue_loaded && steps<200) { cells_queue_step();steps++; }
    assert(!cw.queue_loaded && records==1 && steps>8);
    assert(sleeps>=32); /* Final ownership recheck also leaves real idle gaps. */
    printf("PASS: queue walk yields every 200 ms and completes in %u steps\n",steps);
}
'''
text, n = re.subn(r"int main\(void\) \{.*?\n\}\n(?=''')", lambda m: main, text, count=1, flags=re.S)
assert n == 1
# Run the edited harness; it writes, compiles and runs its own C file.
out = root / 'bench/build/tiles-queue-budget'
out.mkdir(parents=True, exist_ok=True)
script = out / 'run.py'
anchor = "Path(__file__).resolve().parents[2]"
assert anchor in text
script.write_text(text.replace(anchor, f"Path({str(root)!r})"))
subprocess.run([shutil.which('python') or 'python', str(script)], check=True)
