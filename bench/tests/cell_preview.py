import pathlib, re, subprocess
root=pathlib.Path(__file__).resolve().parents[2]
out=root/'bench/out/cell'
out.mkdir(parents=True,exist_ok=True)
build=root/'bench/build'
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
esp_err_t ls_imu_start(void){return 0;}
bool cell_performance_active(void){return getenv("CELL_HIGH")!=NULL;}
void cell_performance_reboot(bool x){printf("REBOOT %d\n",x);}
void cell_monitor_init(void){snprintf(survey.message,sizeof(survey.message),"Select band, then LEARN or START");if(getenv("CELL_LTE_HISTORY")){survey.lte=true;survey.band=0;survey.lte_count=1;survey.cell[0].hz=881500000;survey.cell[0].pci=42;survey.cell[0].hits=4;survey.cell[0].sss=.9;}}
bool cell_monitor_request(int j,unsigned b,bool m){survey.band=b;survey.manual_site=m;survey.busy=j!=CELL_LOAD;survey.lte=j==CELL_LTE;survey.learning=j==CELL_LEARN;return true;}
void cell_monitor_stop(void){survey.busy=false;}
void cell_monitor_wait_stopped(void){cell_monitor_stop();}
void cell_monitor_get(cell_status_t *s){*s=survey;}
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
gcc='C:/mingw64/bin/gcc.exe'
objects=[]
for src in [out/'lssim_cell.c',root/'components/apps/tui/screens/ext/scr_cell.c',root/'components/lakeshark/apps/cell/cell_core.c',out/'cell_stub.c']:
    obj=out/(src.stem+'.obj')
    subprocess.run([gcc,*args,'-c',str(src),'-o',str(obj)],check=True)
    objects.append(str(obj))
line=next(l for l in ninja.splitlines() if l.startswith('build lssim.exe:'))
old=line.split('C_EXECUTABLE_LINKER__lssim_ ',1)[1].split(' |',1)[0].split()
objects+=[str(build/o) for o in old if o!='CMakeFiles/lssim.dir/tools/lssim.c.obj']
libs=[str(build/p) for p in ['lib/liblvgl.a','libcells_sha.a','libcartocore_host.a','libls_lv_probe.a']]
(out/'link.rsp').write_text('\n'.join('"'+x.replace('\\','/')+'"' for x in objects+libs)+'\n-lm\n')
subprocess.run([gcc,'@'+str(out/'link.rsp'),'-o',str(out/'lssim-cell.exe')],check=True)
