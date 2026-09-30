"""Compile the actual worker initializer against allocation-failure fakes."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'components/lakeshark/apps/cell/cell_monitor.c'


def initializer(source):
    start = source.index('void cell_monitor_init(void)')
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if depth == 0:
            return source[start:i + 1]
    raise ValueError('Missing initializer body')


def check_source(source):
    compiler = shutil.which('gcc') or shutil.which('cc')
    if not compiler:
        raise unittest.SkipTest('Host C compiler required')
    harness = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char StackType_t;
typedef struct { int marker; } StaticTask_t;
typedef void *TaskHandle_t;
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static bool endpoint_subscribed;
static int subscriptions, allocations, releases, creates, sub_fail, alloc_fail, task_fail;
static size_t depth;
static unsigned caps;
static void *retained;
static char error[80];
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define tskNO_AFFINITY (-1)
#define pdPASS 1
#define CHECK(x) do { if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 2;} } while(0)
static void task(void *p) {(void)p;}
static void receiver_event(void *p) {(void)p;}
static void message(const char *s) {snprintf(error,sizeof(error),"%s",s);}
static int ls_radio_endpoint_subscribe(void (*fn)(void*),void *arg) {
    (void)fn;(void)arg;subscriptions++;return sub_fail?-1:0;
}
static void *heap_caps_malloc(size_t bytes,unsigned flags) {
    allocations++;caps=flags;if(bytes!=8192)exit(3);
    return alloc_fail?NULL:malloc(bytes);
}
static void heap_caps_free(void *p) {releases++;free(p);}
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void*),const char *name,
    size_t bytes,void *arg,int priority,StackType_t *stack,StaticTask_t *tcb,int core) {
    (void)fn;(void)name;(void)arg;(void)priority;creates++;depth=bytes;
    if(tcb!=&worker_tcb || core!=tskNO_AFFINITY || !stack)exit(4);
    if(task_fail)return NULL;retained=stack;return (void*)1;
}
/* Old dynamic path models the observed low-internal-heap failure. */
static int xTaskCreate(void (*fn)(void*),const char *name,size_t bytes,void *arg,
    int priority,TaskHandle_t *out) {
    (void)fn;(void)name;(void)bytes;(void)arg;(void)priority;*out=NULL;return 0;
}
''' + initializer(source) + r'''
int main(void) {
    sub_fail=1;cell_monitor_init();
    CHECK(!worker && !endpoint_subscribed && allocations==0);
    CHECK(strstr(error,"subscription"));
    sub_fail=0;alloc_fail=1;cell_monitor_init();
    CHECK(!worker && subscriptions==2 && allocations==1 && creates==0);
    CHECK(strstr(error,"allocation"));
    alloc_fail=0;task_fail=1;cell_monitor_init();
    CHECK(!worker && subscriptions==2 && allocations==2 && releases==1 && creates==1);
    task_fail=0;cell_monitor_init();
    CHECK(worker && subscriptions==2 && allocations==3 && creates==2 && releases==1);
    CHECK(depth==8192 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    for(int i=0;i<10;i++)cell_monitor_init();
    CHECK(allocations==3 && creates==2 && subscriptions==2 && releases==1);
    free(retained);puts("subscription failure, allocation failure, task failure, retry and idempotence passed");
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / 'worker.c'
        exe = pathlib.Path(d) / 'worker.exe'
        src.write_text(harness)
        subprocess.run([compiler, '-std=c11', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


class CellWorkerTest(unittest.TestCase):
    def test_allocation_lifecycle(self):
        check_source(SOURCE.read_text())


if __name__ == '__main__':
    unittest.main()
