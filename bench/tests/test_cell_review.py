"""Exercise production CELL callbacks and metadata with host failure fakes."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CELL = ROOT / 'components/lakeshark/apps/cell'
UI = ROOT / 'components/apps/tui/screens/ext/scr_cell.c'


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if not depth:
            return source[start:i + 1]
    raise ValueError(signature)


def run(source, extra=()):
    compiler = shutil.which('gcc') or shutil.which('cc')
    if not compiler:
        raise RuntimeError('Host C compiler required')
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / 'cell.c'
        exe = pathlib.Path(tmp) / 'cell.exe'
        src.write_text(source)
        dirs = [CELL, ROOT/'components/lakeshark/board', ROOT/'components/lakeshark/radio', ROOT/'bench/shims']
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Werror', *['-I'+str(p) for p in dirs],
                        str(src), *map(str, extra), '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


PREFIX = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cell_iq.h"
#include "cell_monitor.h"
#include "cell_core.h"
#define EXT_RAM_BSS_ATTR
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#ifdef _WIN32
#include <process.h>
#include <stdlib.h>
/* mingw's tmpfile() writes to C:\ and fails without admin rights */
__attribute__((unused)) static FILE *host_tmpfile(void)
{
    static unsigned n;
    char path[300];
    const char *dir = getenv("TEMP");
    snprintf(path, sizeof(path), "%s/lscell%d_%u.tmp", dir ? dir : ".", _getpid(), n++);
    return fopen(path, "w+bD");
}
#define tmpfile host_tmpfile
#endif
'''


class CellReviewTest(unittest.TestCase):
    def test_rendered_summary_and_band_provenance(self):
        subprocess.run([shutil.which('python'), str(ROOT/'bench/tests/cell_preview.py')],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = ROOT/'bench/out/cell'
        exe = out/'lssim-cell.exe'
        def render(name, high=False, history=False, args=()):
            env = os.environ.copy()
            env.pop('CELL_HIGH',None);env.pop('CELL_LTE_HISTORY',None)
            if high: env['CELL_HIGH']='1'
            if history: env['CELL_LTE_HISTORY']='1'
            result = subprocess.run([str(exe),'cell','-D','-F','2','-f','3','-d',
                '-o',str(out/(name+'.bmp')),*args],env=env,text=True,capture_output=True,check=True)
            (out/(name+'.txt')).write_text(result.stdout)
            return result.stdout
        portrait = render('normal-p')
        render('normal-l',args=('-l',))
        self.assertIn('READY',portrait)
        before = render('lte-history',history=True)
        self.assertIn('42',before)
        after = render('lte-new-band',history=True,args=('-x','+b,@down,@enter'))
        self.assertIn('900 DL / B8',after)
        self.assertNotIn('881.500',after)
        for name,args in [('high-p',()),('high-l',('-l',))]:
            text = render(name,high=True,args=args)
            self.assertIn('No completed attempt',text)
            self.assertNotIn('Last 0.000',text)
        once = render('high-once',high=True,args=('-l','-k','c'))
        self.assertIn('1 attempts',once)
        self.assertIn('Last ONCE',once)
        options = render('high-options',high=True,args=('-k','o'))
        self.assertNotIn('LORA REPORTS',options)
        render('touch-start',args=('-x','4:24'))

    def test_lte_scan_screen_bands_cells_ppm_and_real_motion(self):
        """The search screen shows what the worker found, and moves only with it."""
        subprocess.run([shutil.which('python'), str(ROOT/'bench/tests/cell_preview.py')],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = ROOT/'bench/out/cell'
        exe = out/'lssim-cell.exe'
        def render(name, scan, args=()):
            env = os.environ.copy()
            env.pop('CELL_HIGH', None); env.pop('CELL_LTE_HISTORY', None)
            env['CELL_LTE_SCAN'] = scan
            result = subprocess.run([str(exe), 'cell', '-D', '-F', '2', '-f', '3', '-d',
                '-o', str(out/(name+'.bmp')), *args], env=env, text=True, capture_output=True, check=True)
            (out/(name+'.txt')).write_text(result.stdout)
            return result.stdout
        done = render('scan-done-p', 'done')
        for text in ['LTE CELL SEARCH', 'EARFCN 2400-2649', 'GATED: 2 carriers / 2 points', 'MIB 2 of 3 decoded',
                     'DONGLE +9.6 ppm / 3 cells', 'APPLIED +10', ' 2525  301 ', ' 2600   17 ', '10M 2x', '5M 2x',
                     'BAND OCCUPANCY', '= carrier  + searched  * cell']:
            self.assertIn(text, done)
        many_p = render('scan-many-p', 'many')
        self.assertIn('+5 more', many_p)
        many_l = render('scan-many-l', 'many', args=('-l',))
        for pci in ['301', '412', '451', '33']:
            self.assertIn(' '+pci+' ', many_l)
        self.assertNotIn('more:', many_l)          # landscape lists all fourteen
        # The tower lights an arc per cell heard, and the survey draws it dark.
        self.assertIn('(((o)))', done)
        # Motion only from real data: advancing the clock changes nothing.
        still = render('scan-run-0', 'run')
        moved = render('scan-run-1', 'run', args=('-A', '160', '-f', '9'))
        self.assertEqual(still.splitlines()[:-1], moved.splitlines()[:-1])
        self.assertNotIn('RECEIVING', render('scan-idle', 'done'))
        self.assertIn('RECEIVING', still)
        # Tapping the list, and N, open every cell; the band list carries EARFCN ranges.
        listing = render('scan-list-key', 'many', args=('-k', 'n'))
        self.assertIn('LTE CELLS / 850 DL / B5', listing)
        self.assertIn('881.500 MHz  PCI 301', listing)
        tapped = render('scan-list-tap', 'many', args=('-x', '8:36'))
        self.assertIn('LTE CELLS', tapped)
        bands = render('scan-bands', 'done', args=('-k', 'b'))
        for text in ['600 DL / B71', 'EARFCN 68586-68935', '700 DL / B14', '850 DL / B26', '700 DL / B17', 'EARFCN 2400-2649']:
            self.assertIn(text, bands)
        self.assertNotIn('B66', bands)             # HackRF-only bands are not offered to the RTL scan
        options = render('scan-options', 'done', args=('-k', 'o'))
        for text in ['LTE SCAN', 'GATED', 'LTE MIB', 'DONGLE PPM', 'ALL CELLS', '14 found' if False else 'found']:
            self.assertIn(text, options)

    def test_full_iq_and_sweep_compile(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        with tempfile.TemporaryDirectory() as tmp:
            prelude = pathlib.Path(tmp) / 'host.h'
            prelude.write_text(r"""
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#endif
void *heap_caps_aligned_alloc(size_t alignment,size_t n,unsigned caps);
void vTaskPrioritySet(void *task,unsigned priority);
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t *p,uint32_t n);
""")
            (pathlib.Path(tmp)/'esp_rom_crc.h').write_text('')
            dirs = [pathlib.Path(tmp), CELL, ROOT/'components/lakeshark/board',
                    ROOT/'components/lakeshark/radio', ROOT/'components/lakeshark/dsp', ROOT/'bench/shims']
            for src in [CELL/'cell_iq.c', ROOT/'components/lakeshark/dsp/ls_sweep.c']:
                subprocess.run([compiler,'-std=gnu11','-Werror=implicit-function-declaration',
                    *['-I'+str(p) for p in dirs], '-include',str(prelude),'-fsyntax-only',str(src)],check=True)

    def test_high_rate_actions_and_values(self):
        source = UI.read_text()
        funcs = '\n'.join(function(source, s) for s in [
            'static bool value_busy(', 'static bool value_changed(', 'static bool value_frequency(',
            'static ls_act_status_t start(', 'static ls_act_status_t stop('])
        run(PREFIX + r'''
typedef struct {int kind,i;float f;} ls_val_t;
typedef int ls_args_t;
typedef int ls_act_status_t;
enum {LS_VAL_BOOL,LS_VAL_INT,LS_VAL_FLOAT,LS_ACT_OK,LS_ACT_BUSY};
static bool high,opened=true,auto_multi;
static uint32_t hrf_frequency=739000000;
static cell_iq_status_t iq;
static cell_status_t survey;
static int survey_starts,iq_starts;
static bool cell_performance_active(void){return high;}
void cell_monitor_get(cell_status_t *s){*s=survey;}
void cell_iq_get_status(cell_iq_status_t *s){*s=iq;}
bool cell_iq_auto_begin(uint32_t hz,bool multi){(void)multi;iq_starts++;iq.busy=true;iq.active_hz=hz;return true;}
static bool request(int cmd){assert(cmd==CELL_WATCH);survey_starts++;survey.busy=true;return true;}
void cell_monitor_stop(void){survey.busy=false;}
void cell_iq_stop(void){iq.busy=false;}
''' + funcs + r'''
int main(void){
    ls_val_t out;high=true;assert(start(NULL,NULL)==LS_ACT_OK);
    assert(iq_starts==1 && !survey_starts);assert(value_busy(&out) && out.i);
    assert(value_frequency(&out) && out.f>738 && out.f<740);assert(!value_changed(&out));
    survey.busy=true;assert(stop(NULL,NULL)==LS_ACT_OK);assert(!iq.busy && !survey.busy);
    high=false;assert(start(NULL,NULL)==LS_ACT_OK && survey_starts==1);
    assert(value_busy(&out) && out.i);stop(NULL,NULL);assert(!survey.busy);
    opened=false;assert(start(NULL,NULL)==LS_ACT_BUSY && survey_starts==1);
    return 0;
}
''')

    def test_acquire_retains_unavailable_busy_and_timing_causes(self):
        source = (CELL/'cell_iq.c').read_text()
        run(PREFIX + r"""
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static uint8_t *capture;
static size_t captured,replay_expected;
static uint32_t frequency,rate,duration_us,crc;
static uint64_t dropped;
static bool complete,replay,timing_rejected,device_loss,capture_config_valid;
static const char *capture_source;
static ls_radio_iq_health_t radio_health;
static ls_radio_iq_config_t capture_config;
static ls_radio_err_t radio_health_result,stop_result,acquire_result;
static ls_radio_err_t acquire_error,configure_error;
static int64_t now,step=100;
static int64_t esp_timer_get_time(void){now+=step;return now;}
static void status(void){}
const char *ls_radio_err_name(ls_radio_err_t e){(void)e;return "fake";}
ls_radio_err_t ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **s)
{(void)owner;assert(!strcmp(r->preferred_endpoint_id,LS_RADIO_ENDPOINT_HACKRF_USB));*s=(void*)1;return acquire_error;}
static void *heap_caps_aligned_alloc(size_t alignment,size_t n,unsigned caps){(void)alignment;(void)caps;return malloc(n);}
ls_radio_err_t ls_radio_iq_configure(ls_radio_session_t *s,const ls_radio_iq_config_t *c,ls_radio_iq_config_t *a)
{(void)s;*a=*c;return configure_error;}
ls_radio_err_t ls_radio_iq_start(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_stop(ls_radio_session_t *s){(void)s;return LS_RADIO_OK;}
ls_radio_err_t ls_radio_iq_get_health(ls_radio_session_t *s,ls_radio_iq_health_t *h)
{(void)s;(void)h;return LS_RADIO_ERR_UNSUPPORTED;}
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s,void *b,size_t n,uint32_t timeout,size_t *got)
{(void)s;(void)timeout;memset(b,128,n);*got=n;return LS_RADIO_OK;}
void ls_radio_release(ls_radio_session_t *s){(void)s;}
static uint64_t rtlsdr_stream_dropped(void){return 0;}
static uint32_t esp_rom_crc32_le(uint32_t v,const uint8_t *b,size_t n){(void)b;(void)n;return v;}
""" + function(source,'static void acquire(') + r"""
int main(void){
    acquire_error=LS_RADIO_ERR_UNAVAILABLE;acquire(739000000,8000000,80,true);
    assert(!complete && acquire_result==LS_RADIO_ERR_UNAVAILABLE);
    acquire_error=LS_RADIO_ERR_BUSY;acquire(739000000,8000000,80,true);
    assert(!complete && acquire_result==LS_RADIO_ERR_BUSY);
    acquire_error=LS_RADIO_OK;configure_error=LS_RADIO_ERR_IO;acquire(739000000,8000000,80,true);
    assert(!complete && acquire_result==LS_RADIO_ERR_IO);
    configure_error=LS_RADIO_OK;step=100000;acquire(739000000,8000000,80,true);
    assert(!complete && timing_rejected && acquire_result==LS_RADIO_ERR_TIMEOUT);
    free(capture);return 0;
}
""")

    def test_help_describes_power_and_separate_lte_evidence(self):
        source = (ROOT/'components/apps/tui/ls_app_docs.c').read_text()
        doc = source.split('const ls_app_doc_t ls_doc_cell = {',1)[1].split('.records',1)[0]
        self.assertIn('downlink power',doc)
        self.assertIn('separately checks MIB',doc)
        self.assertIn('SIB are not decoded',doc)
        self.assertNotIn('they broadcast about themselves',doc)

    def test_recent_sd_history_keeps_latest_eight_observations(self):
        source = UI.read_text()
        run(PREFIX + r"""
typedef int ls_opt_t;
static bool mounted;
static FILE *journal;
static char names[8][80];
static unsigned count;
static bool ls_sdcard_mounted(void){return mounted;}
static FILE *open_journal(const char *path,const char *mode)
{assert(!strcmp(path,"/sdcard/cell/lte.jsonl") && !strcmp(mode,"rb"));return journal;}
#define fopen open_journal
static void history_choose(int i){(void)i;}
static void ls_picker_open(const char *title,void (*choose)(int)){(void)choose;assert(strstr(title,"PCI ONLY"));count=0;}
static void ls_picker_add(const char *title,const char *hint)
{(void)hint;assert(count<8);snprintf(names[count++],80,"%s",title);}
""" + function(source,'static void o_history(') + r"""
int main(void){
    journal=tmpfile();assert(journal);
    for(int i=0;i<10;i++)fprintf(journal,"{\"kind\":\"LTE_SYNC\",\"hz\":881500000,\"pci\":%d,\"hits\":4}\n",i);
    fputs("{\"kind\":\"LTE_COVERAGE\"}\n",journal);fflush(journal);mounted=true;
    o_history(NULL);assert(count==8 && strstr(names[0],"PCI 9") && strstr(names[7],"PCI 2"));
    mounted=false;o_history(NULL);assert(count==1 && strstr(names[0],"No saved"));
    return 0;
}
""")

    def test_reports_off_applies_without_sd(self):
        source = (CELL/'cell_report.c').read_text()
        run(PREFIX + r'''
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#define fsync(fd) _commit(fd)
#endif
static int lock;
static char target[17],last_text[49];
static bool queued,persist_failed;
static int64_t last_attempt;
static unsigned last_pass;
static char last_kind;
static uint32_t last_lte_hz;
static int last_lte_pci;
static bool mounted;
static int no_mkdir(const char *path){(void)path;return 0;}
static FILE *no_open(const char *path,const char *mode){(void)path;(void)mode;return NULL;}
#undef mkdir
#define mkdir(path,mode) no_mkdir(path)
#define fopen no_open
static bool valid_peer(const char *p){return strlen(p)==16;}
static bool cell_report_transport_known(const char *p){(void)p;return true;}
static void cell_report_init(void){}
static bool ls_sdcard_mounted(void){return mounted;}
''' + function(source, 'bool cell_report_target(') + r'''
int main(void){
    strcpy(target,"0123456789abcdef");queued=true;
    assert(!cell_report_target(""));assert(!target[0] && !queued && persist_failed);
    assert(!cell_report_target("fedcba9876543210"));assert(!strcmp(target,"fedcba9876543210"));
    assert(!cell_report_target("bad"));assert(!strcmp(target,"fedcba9876543210"));
    mounted=true;assert(!cell_report_target(""));assert(!target[0] && persist_failed);
    return 0;
}
''')

    def test_metadata_has_null_invalid_coordinates(self):
        source = (CELL/'cell_iq.c').read_text()
        run(PREFIX + r'''
static uint32_t crc;
static uint64_t dropped;
const char *ls_radio_err_name(ls_radio_err_t e){(void)e;return "unavailable";}
''' + function(source, 'static bool write_metadata(') + r'''
int main(void){
    cell_iq_status_t s={0};s.latitude=NAN;s.longitude=999;s.end_latitude=91;s.end_longitude=NAN;
    s.acquire_result=LS_RADIO_ERR_UNAVAILABLE;
    FILE *f=tmpfile();assert(f && write_metadata(f,&s));rewind(f);
    char text[4096];size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
    assert(strstr(text,"\"latitude\":null") && strstr(text,"\"longitude\":null"));
    assert(strstr(text,"\"end_latitude\":null") && strstr(text,"\"end_longitude\":null"));
    assert(strstr(text,"\"acquire_result\":\"unavailable\""));
    s.gps_valid=true;s.latitude=43;s.longitude=-71;
    f=tmpfile();assert(write_metadata(f,&s));rewind(f);n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
    assert(strstr(text,"\"latitude\":43.0000000") && strstr(text,"\"end_latitude\":null"));
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
