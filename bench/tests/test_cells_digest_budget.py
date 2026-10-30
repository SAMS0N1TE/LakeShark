"""Exercise the production hash loop with slow reads and an inherited-priority sleep seam."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'cell').write_bytes(b'x' * (3 * 32768 + 17))
source = (root / 'components/apps/tui/ls_cells_worker.h').read_text()
pause = source[source.index('static void cells_pause('):source.index('#define CELLS_ROOT')]
digest = source[source.index('static bool cells_digest('):source.index('static bool cells_hash(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
static struct { uint8_t buf[32768]; } cw;
static int64_t now,last_sleep;
static unsigned sleeps,reads,fail_at;
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(unsigned ticks) {
    assert(ticks>=2 && now-last_sleep<=35000);sleeps++;last_sleep=now;
}
static int slow_read(int fd,void *buf,unsigned n) {
    now+=35000;reads++;if(reads==fail_at)return -1;return read(fd,buf,n);
}
#define read slow_read
/* SHA correctness is covered by the real mbedTLS download transaction test. */
typedef struct { uint64_t bytes; } mbedtls_sha256_context;
static void mbedtls_sha256_init(mbedtls_sha256_context *c) { c->bytes=0; }
static int mbedtls_sha256_starts(mbedtls_sha256_context *c,int mode) { (void)c;(void)mode;return 0; }
static int mbedtls_sha256_update(mbedtls_sha256_context *c,const uint8_t *buf,size_t n) {
    assert(n<=32768);for(size_t i=0;i<n;i++)assert(buf[i]=='x');c->bytes+=n;return 0;
}
static int mbedtls_sha256_finish(mbedtls_sha256_context *c,uint8_t *digest) { memset(digest,0,32);memcpy(digest,&c->bytes,8);return 0; }
static void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
''' + pause + digest + r'''
static void reset(void) { now=last_sleep=0;sleeps=reads=fail_at=0; }
int main(void) {
    const uint64_t size=3*32768+17;uint8_t digest[32];uint64_t counted;
    assert(cells_digest("cell",size,digest));memcpy(&counted,digest,8);
    assert(counted==size && sleeps==4 && reads==5);
    reset();assert(!cells_digest("cell",size+1,digest) && sleeps==4);
    reset();assert(!cells_digest("cell",size-1,digest) && sleeps==4);
    reset();fail_at=2;assert(!cells_digest("cell",size,digest) && sleeps==1);
    reset();assert(!cells_digest("missing",size,digest) && !reads);
    puts("PASS: hash sleeps at least two ticks under slow reads; exact size and read failures remain strict");
}
'''
(out / 'test.c').write_text(code)
exe = out / 'test.exe'
subprocess.run([sys.argv[1], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], cwd=out, check=True)
