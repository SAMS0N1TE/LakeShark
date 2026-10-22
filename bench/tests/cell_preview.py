import os, pathlib, re, shutil, subprocess
root=pathlib.Path(__file__).resolve().parents[2]
out=root/'bench/out/cell'
out.mkdir(parents=True,exist_ok=True)
build=root/os.environ.get('LS_CELL_BUILD','bench/build')
ninja=(build/'build.ninja').read_text()
block=ninja.split('build CMakeFiles/lssim.dir/tools/lssim.c.obj:',1)[1].split('\nbuild ',1)[0]
args=[]
for name in ['DEFINES','INCLUDES','FLAGS']:
    line=re.search(r'^  '+name+r' = (.*)$',block,re.M).group(1)
    args+=line.replace('\\"','"').split()
args+=['-I'+str(root/'components/lakeshark/apps/cell')]
source=(root/'bench/tools/lssim.c').read_text()
source=source.replace('static const ls_app_t APPS[] = {','extern const ls_tui_screen_t ls_scr_cell;\nstatic const ls_app_t APPS[] = {\n {"cell","CELL WATCH","cell survey",LS_ICON_TOWER,TUI_CYAN,LS_APP_MAIN,&ls_scr_cell,NULL,&ls_doc_cell},')
source=source.replace('ls_tui_set_font_index(ls_tui_font_for_view(font, APPS[idx].screen->name, false));','ls_tui_set_font_index(getenv("CELL_HIGH")?font:ls_tui_font_for_view(font, APPS[idx].screen->name, false));')
source=source.replace('int rc = ls_app_register(&APPS[i]);','if(getenv("CELL_HIGH") && strcmp(APPS[i].id,"cell"))continue; int rc = ls_app_register(&APPS[i]);')
source=source.replace('void lssim_set_empty(bool on);','ls_scr_cell.enter();\n    void lssim_set_empty(bool on);')
(out/'lssim_cell.c').write_text(source)
stub=r'''
#include "cell_monitor.h"
#include "cell_iq.h"
#include "cell_report.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static cell_status_t survey;
static cell_iq_status_t iq;
static cell_lte_t cells[CELL_MAX_CELLS];
static unsigned cell_count;
/* CELL_LTE_SCAN=run|done|many seeds a gated B5 search: its occupancy trace, the
 * carriers, the cells with power and MIB, and the dongle clock. */
static void seed_scan(const char *how){
    survey.lte=true;survey.band=0;survey.gated=true;survey.mib_on=true;
    survey.spectrum_count=96;
    for(int i=0;i<96;i++){
        int v=-86+(i*7%5)-2;
        if(i>=31&&i<65)v=-55+(i*3%5)-2;
        if(i>=69&&i<85)v=-62+(i*5%5)-2;
        survey.spectrum[i]=(int8_t)v;
        survey.reference[i]=CELL_MISSING;
        survey.mark[i]=(i>=31&&i<65)||(i>=69&&i<85)?1:0;
    }
    survey.plateaus=2;survey.candidates=2;survey.tunes=2;
    survey.ppm_known=true;survey.ppm=9.6f;survey.ppm_spread=.4f;survey.ppm_cells=3;
    if(!strcmp(how,"run")){
        survey.busy=true;survey.phase=CELL_PH_SYNC;survey.tune=1;survey.frequency=881500000;survey.scan_ms=14300;
        survey.lte_attempted=1;survey.lte_usable=1;survey.mark[48]|=2;survey.ppm_known=false;survey.ppm_cells=0;
        snprintf(survey.message,sizeof(survey.message),"LTE: matching repeated PSS and SSS");
        return;
    }
    survey.tune=2;survey.scan_ms=38200;survey.frequency=889000000;survey.lte_attempted=2;survey.lte_usable=2;
    survey.ppm_applied=true;survey.ppm_value=10;survey.mib_tried=3;survey.mib_done=2;
    survey.mark[48]|=6;survey.mark[77]|=6;
    snprintf(survey.message,sizeof(survey.message),"LTE pass (occupied carriers) done; no verdict");
    struct {uint32_t hz;uint16_t earfcn,pci;int16_t rsrp,sfn;uint8_t rb,ports,mib;} rows[]={
        {881500000,2525,301,-452,446,50,2,CELL_MIB_OK},{881500000,2525,302,-598,0,0,0,CELL_MIB_FAILED},
        {889000000,2600,17,-661,212,25,2,CELL_MIB_OK},{879000000,2500,88,-703,0,0,0,CELL_MIB_NONE},
        {886000000,2570,412,-501,18,75,4,CELL_MIB_OK},{874500000,2445,5,-540,0,0,0,CELL_MIB_FAILED},
        {892000000,2630,263,-648,901,6,1,CELL_MIB_OK},{870500000,2405,140,-725,0,0,0,CELL_MIB_NONE},
        {883500000,2545,77,-480,331,100,2,CELL_MIB_OK},{877000000,2480,190,-690,0,0,0,CELL_MIB_FAILED},
        {890500000,2615,350,-585,640,15,2,CELL_MIB_OK},{872000000,2430,33,-760,0,0,0,CELL_MIB_NONE},
        {885000000,2560,451,-512,77,50,2,CELL_MIB_OK},{878500000,2495,222,-670,0,0,0,CELL_MIB_FAILED},
    };
    unsigned n=!strcmp(how,"many")?14:3;
    for(unsigned i=0;i<n;i++){
        cells[i]=(cell_lte_t){.hz=rows[i].hz,.earfcn=rows[i].earfcn,.pci=rows[i].pci,.rsrp10=rows[i].rsrp,
            .sfn=rows[i].sfn,.n_rb=rows[i].rb,.ports=rows[i].ports,.mib=rows[i].mib,.hits=4,.sss=.8f};
    }
    cell_count=n;survey.last.hz=889000000;survey.last.pci=17;
}
esp_err_t ls_imu_start(void){return 0;}
bool cell_performance_active(void){return getenv("CELL_HIGH")!=NULL;}
void cell_performance_reboot(bool x){printf("REBOOT %d\n",x);}
void cell_monitor_init(void){
    snprintf(survey.message,sizeof(survey.message),"Select band, then LEARN or START");
    survey.gated=true;survey.mib_on=true;
    if(getenv("CELL_LTE_HISTORY")){survey.lte=true;survey.band=0;cell_count=1;cells[0]=(cell_lte_t){.hz=881500000,.earfcn=2525,.pci=42,.hits=4,.sss=.9f,.rsrp10=CELL_NO_POWER};}
    if(getenv("CELL_LTE_SCAN"))seed_scan(getenv("CELL_LTE_SCAN"));
}
unsigned cell_monitor_cells(cell_lte_t *out,unsigned max){unsigned n=cell_count<max?cell_count:max;memcpy(out,cells,n*sizeof(cells[0]));return n;}
void cell_monitor_set_full_scan(bool full){survey.gated=!full;}
void cell_monitor_set_mib(bool on){survey.mib_on=on;}
bool cell_monitor_clear_ppm(void){survey.ppm_known=false;return true;}
bool cell_monitor_request(int j,unsigned b,bool m){survey.band=b;survey.manual_site=m;survey.busy=j!=CELL_LOAD;survey.lte=j==CELL_LTE;survey.learning=j==CELL_LEARN;return true;}
void cell_monitor_stop(void){survey.busy=false;}
void cell_monitor_wait_stopped(void){cell_monitor_stop();}
void cell_monitor_get(cell_status_t *s){*s=survey;s->lte_count=cell_count;}
void cell_report_init(void){}
int cell_report_command(int n,char **args){(void)n;(void)args;return 0;}
bool cell_report_target(const char *s){return false;}
void cell_report_status(char *s,size_t n){snprintf(s,n,"LoRa: off");}
bool cell_iq_auto_begin(uint32_t h,bool m){iq.busy=true;iq.automatic=true;iq.active_hz=h;iq.multi=m;return true;}
bool cell_iq_hackrf_begin(uint32_t h,uint32_t r,uint32_t ms){iq.hz=h;iq.rate=r;iq.requested_ms=ms;iq.attempts=1;iq.decoded=1;iq.complete=true;iq.lte_found=true;iq.mib_found=true;iq.lte.pci=42;iq.mib.frames=2;return true;}
void cell_iq_stop(void){iq.busy=false;}
void cell_iq_get_status(cell_iq_status_t *s){*s=iq;}
'''
(out/'cell_stub.c').write_text(stub)
cache = build/'CMakeCache.txt'
if cache.exists():
    compiler = re.search(r'^CMAKE_C_COMPILER:[^=]+=(.+)$', cache.read_text(), re.M)
    if not compiler:
        raise RuntimeError(f'{cache} has no configured CMAKE_C_COMPILER')
    gcc = compiler.group(1).strip()
    if not pathlib.Path(gcc).is_file():
        raise RuntimeError(f'Configured host C compiler is unavailable: {gcc}')
else:
    gcc = shutil.which('gcc')
    if not gcc:
        raise RuntimeError('Host gcc required when no CMake cache is available')
objects=[]
for src in [out/'lssim_cell.c',root/'components/apps/tui/screens/ext/scr_cell.c',root/'components/lakeshark/apps/cell/cell_core.c',out/'cell_stub.c']:
    obj=out/(src.stem+'.obj')
    subprocess.run([gcc,*args,'-c',str(src),'-o',str(obj)],check=True)
    objects.append(str(obj))
line=next(l for l in ninja.splitlines() if l.startswith('build lssim.exe:'))
old=line.split('C_EXECUTABLE_LINKER__lssim_ ',1)[1].split(' |',1)[0].split()
objects+=[str(build/o) for o in old if o!='CMakeFiles/lssim.dir/tools/lssim.c.obj']
libs=[str(build/p) for p in ['libcells_sha.a','libcartocore_host.a']]   # LVGL went in 2.17.0
(out/'link.rsp').write_text('\n'.join('"'+x.replace('\\','/')+'"' for x in objects+libs)+'\n-lm\n')
subprocess.run([gcc,'@'+str(out/'link.rsp'),'-o',str(out/'lssim-cell.exe')],check=True)
