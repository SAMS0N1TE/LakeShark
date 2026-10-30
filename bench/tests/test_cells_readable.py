"""Compile production cell admission/open paths against stale SD fixtures."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[2]).resolve()
cells = out / 'maps/cells'
cells.mkdir(parents=True, exist_ok=True)
old = b'x' * 64
new = b'y' * 64
index = b'i' * 64
for name, data in [('8-0-0.ctile', old), ('8-0-1.ctile', old),
                   ('8-0-2.ctile', old), ('short.ctile', old[:-1]),
                   ('index.cci', index)]:
    path = cells / name
    path.write_bytes(data)
    Path(str(path) + '.sha').write_text(
        f'{len(data)} {int(path.stat().st_mtime)} {hashlib.sha256(data).hexdigest()}\n')
for direct in (False, True):
    header = bytearray(128)
    header[:8] = b'CCMAPS2\0' if direct else b'CCMAPS1\0'
    struct.pack_into('<I', header, 8, 2)
    rows = b''
    for ident, data in [(1, new), (2, new + b'new revision')]:
        rows += struct.pack('<II', ident, len(data)) + hashlib.sha256(data).digest()
        if direct:
            rows += struct.pack('<II', 0, 1)
    (cells / ('direct.ccm' if direct else 'indexed.ccm')).write_bytes(header + rows)

worker = (root / 'components/apps/tui/ls_cells_worker.h').read_text()
reader = (root / 'components/apps/tui/ls_cartocore.c').read_text()
entry = worker[worker.index('static bool cells_entry_checked('):worker.index('/* CCMAPS2 has')]
matches = worker[worker.index('static bool cells_cached_digest('):worker.index('/* Failure records survive')]
present = worker[worker.index('static void cells_name('):worker.index('#include "ls_cells_states_worker.h"')]
opening = reader[reader.index('static int cell_open('):reader.index('static double centre_lat=')]
u32 = worker[worker.index('static uint32_t cells_u32('):worker.index('\n', worker.index('static uint32_t cells_u32('))]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <direct.h>
#include "ls_carto_digest.h"
#include "ls_carto_sd_io.h"
#define CELLS_ROOT "maps/"
#define LS_SNPRINTF(tag,...) snprintf(__VA_ARGS__)
#define heap_caps_malloc(n,caps) malloc(n)
static struct {
    bool initialized,direct;int catalog;uint32_t entries,stride,index_size;uint8_t index_sha[32];
} cw;
static bool cells_direct_active(void) { return cw.direct; }
static bool allow_hash,change_mtime;
static unsigned hashes;
static uint8_t old_sha[32];
/* Cached checks must never reach the hash; unknown records use this seam. */
static bool cells_digest(const char *p,uint64_t n,uint8_t sha[32]) {
    (void)n;assert(allow_hash);hashes++;memcpy(sha,old_sha,32);
    if(change_mtime) {
        struct stat st;assert(!stat(p,&st));struct utimbuf t={st.st_atime,st.st_mtime+4};assert(!utime(p,&t));
    }
    return true;
}
typedef struct { char path[128]; } sd_source;
typedef struct { int fd;uint64_t size;sd_source *owner; } sd_cell_file;
static size_t source_handles_live,source_handles_hw;
/* The worker reads catalog rows from PSRAM when it can; this test reads the card. */
static bool cells_catalog_read(uint64_t off,void *dst,size_t n) {
    return carto_sd_read_at(cw.catalog,128+(uint64_t)cw.entries*cw.stride,off,dst,n,NULL,0);
}
''' + u32 + entry + present + matches + opening + r'''
static void opened(sd_source *owner,uint32_t id) {
    void *handle=NULL;uint64_t bytes=0;
    assert(cell_open(owner,id,&handle,&bytes)==1 && bytes==64);
    sd_cell_file *f=handle;uint8_t data[64];
    assert(f->size==64 && read(f->fd,data,64)==64);
    for(unsigned i=0;i<64;i++)assert(data[i]=='x');
    close(f->fd);source_handles_live-=sizeof(*f);free(f);
}
int main(void) {
    cw.initialized=true;cw.entries=2;cw.index_size=64;
    FILE *old=fopen("maps/cells/8-0-0.ctile.sha","r");assert(old);
    long long old_size,old_mtime;char old_hex[65];assert(carto_digest_scan(old,&old_size,&old_mtime,old_hex)==3);fclose(old);
    for(unsigned i=0;i<32;i++) { unsigned v;assert(sscanf(old_hex+i*2,"%2x",&v)==1);old_sha[i]=(uint8_t)v; }
    FILE *f=fopen("maps/cells/index.cci.sha","r");assert(f);
    long long size,mtime;char hex[65];assert(carto_digest_scan(f,&size,&mtime,hex)==3);fclose(f);
    for(unsigned i=0;i<32;i++) { unsigned v;assert(sscanf(hex+i*2,"%2x",&v)==1);cw.index_sha[i]=(uint8_t)v; }
    sd_source owner={.path="maps/cells/catalog.ccm"};
    for(unsigned direct=0;direct<2;direct++) {
        cw.direct=direct;cw.stride=direct?48:40;
        cw.catalog=open(direct?"maps/cells/direct.ccm":"maps/cells/indexed.ccm",O_RDONLY);assert(cw.catalog>=0);
        assert(cells_matches("maps/cells/index.cci",cw.index_size,cw.index_sha));
        /* Same-size SHA mismatch and different-size revision both stay outdated. */
        for(uint32_t id=1;id<=2;id++) {
            uint32_t expected;uint8_t sha[32];char path[96];
            snprintf(path,sizeof(path),"maps/cells/8-0-%u.ctile",id-1);
            assert(cells_entry(id,&expected,sha) && !cells_matches(path,expected,sha));
            assert(!cells_present(id,expected));
            assert(cells_file_current(id,path)==(bool)direct);
            if(direct)opened(&owner,id);
            else { void *handle=NULL;uint64_t bytes=0;assert(cell_open(&owner,id,&handle,&bytes)==0); }
        }
        void *handle=NULL;uint64_t bytes=0;
        assert(!cells_file_current(3,"maps/cells/8-0-2.ctile"));
        assert(cell_open(&owner,3,&handle,&bytes)==0);
        assert(!cells_file_current(1,"maps/cells/missing.ctile"));
        assert(!cells_file_current(1,"maps/cells/short.ctile"));
        assert(!cells_file_current(1,"maps/cells"));
        close(cw.catalog);cw.catalog=-1;
    }
    /* Uncached stale direct reads still need only stat, even on repeated opens. */
    assert(!unlink("maps/cells/8-0-0.ctile.sha"));
    cw.catalog=open("maps/cells/direct.ccm",O_RDONLY);assert(cw.catalog>=0);
    opened(&owner,1);opened(&owner,1);
    close(cw.catalog);cw.catalog=-1;
    assert(!cells_file_current(1,"maps/cells/8-0-0.ctile"));
    assert(!source_handles_live);
    const char *path="maps/cells/8-0-0.ctile";uint8_t different[32]={0};bool error=false;
    assert(!hashes);allow_hash=true;
    /* Record mismatches as well as matches; a shared stale cell hashes once. */
    assert(!cells_matches_checked(path,64,different,&error) && !error && hashes==1);
    assert(!cells_matches(path,64,different) && cells_matches(path,64,old_sha) && hashes==1);
    struct stat st;assert(!stat(path,&st));struct utimbuf t={st.st_atime,st.st_mtime+4};assert(!utime(path,&t));
    assert(cells_matches(path,64,old_sha) && hashes==2);
    assert(cells_matches(path,64,old_sha) && hashes==2);
    /* An unwritable sidecar cannot grant cached trust on the next check. */
    assert(carto_digest_remove(path));assert(!_mkdir("maps/cells/8-0-0.ctile.sha.part"));
    assert(cells_matches(path,64,old_sha) && hashes==3);
    assert(cells_matches(path,64,old_sha) && hashes==4);
    assert(!_rmdir("maps/cells/8-0-0.ctile.sha.part"));
    assert(cells_matches(path,64,old_sha) && hashes==5);
    assert(cells_matches(path,64,old_sha) && hashes==5);
    assert(carto_digest_remove(path));change_mtime=true;
    assert(!cells_matches_checked(path,64,old_sha,&error) && error && hashes==6);
    assert(stat("maps/cells/8-0-0.ctile.sha",&st));
    puts("PASS: recorded match/mismatch never rehashed; missing record, mtime and write failure remain strict");
    puts("PASS: direct stale listed cells readable without hashing; real size returned; unlisted/short/nonregular rejected; indexed stale rejected; strict presence unchanged");
}
'''
(out / 'test.c').write_text(code)
exe = out / 'test.exe'
subprocess.run([sys.argv[1], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'components/apps/tui'),
                str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], cwd=out, check=True)
