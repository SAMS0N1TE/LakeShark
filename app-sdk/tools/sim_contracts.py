"""Generate diagnostics/path adapters; pointer lifetime rules run in shared firmware.
Synchronous inputs/outputs use api_readable; retained SD outputs use api_owns.
"""
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import apidoc


def generate(output):
    code = ['#include "lakeshark_app.h"', '#include "lakeshark_ui.h"', '#include "lap3d.h"',
            '#include "ls_lap_radio.h"', '#include "ls_lap_api.h"', '#include <stdio.h>', '#include <string.h>', '#include "ls_lap_sys.h"',
            'const char *ls_lap_sim_call = "runtime";']
    names = []
    for e in apidoc.entries():
        if not e.get('exported') or not e['name'].startswith('lap_') or not e['signature'] or '...' in e['signature']: continue
        n = e['name']; sig = e['signature']; ret = sig[:sig.index(n)].strip()
        args = sig[sig.index('(')+1:-1]
        # Split commas only outside nested callback declarations.
        parts = re.split(r',\s*(?![^()]*\))', args)
        argv = []
        for a in parts:
            if a.strip() in ('void', ''): continue
            cb = re.search(r'\(\s*\*\s*(\w+)\s*\)', a)
            argv.append(cb.group(1) if cb else re.findall(r'(\w+)\s*(?:\[[^]]*\])?$', a)[0])
        code += [sig.replace(n, '__real_' + n, 1) + ';', sig.replace(n, '__wrap_' + n, 1) + ' {',
                 ' const char *previous = ls_lap_sim_call; ls_lap_sim_call = "%s";' % n]
        if e['simulator']['status'] in ('faked', 'limited', 'missing'):
            line = 'simulator limit: %s: %s\n' % (n, e['simulator']['reason'])
            import json
            code += [' static int warned; if (!warned) { warned=1; fputs(%s, stderr); }' % json.dumps(line)]
        if n in ('lap_mesh_send', 'lap_mesh_dm', 'lap_mesh_advert'):
            code += [' if (!(ls_lap_granted() & LAP_CAP_RADIO_TX)) { fprintf(stderr, "simulator contract: %s permission denied (LAP_CAP_RADIO_TX)\\n"); ls_lap_sim_call=previous; return false; }' % n]
        if n in ('lap_mesh_send', 'lap_mesh_dm', 'lap_mesh_advert'):
            code += [' if (!ls_lap_sys_host()->self.running || !ls_lap_sys_host()->self.tx_armed) { ls_lap_sim_call=previous; return false; }']
        if n.startswith('lap_sd_') and n != 'lap_sd_poll':
            arg = 'dir' if n == 'lap_sd_list' else 'path'
            code += [' char host_path[128]; size_t path_len=0; bool valid_path=false;',
                     ' for (;path_len<sizeof(host_path);path_len++) { if (!ls_lap_api_readable(%s+path_len,1)) break; host_path[path_len]=%s[path_len]; if (!host_path[path_len]) { valid_path=true; break; } }' % (arg,arg),
                     ' if (!valid_path || strncmp(host_path,"/sdcard",7) || (host_path[7] && host_path[7]!=47)) { fprintf(stderr,"simulator contract: %s EINVAL: expected /sdcard path\\n"); ls_lap_sim_call=previous; return LAP_SD_EINVAL; }' % n,
                     ' %s=host_path+1; /* private host sdcard directory; runtime copies path */' % arg]
        call = '__real_%s(%s)' % (n, ', '.join(argv))
        code += [' ' + (call + ';' if ret == 'void' else ret + ' result = ' + call + ';')]
        system_call = n.startswith(('lap_ipc_', 'lap_launch_', 'lap_stream_', 'lap_storage_', 'lap_dir_', 'lap_file_picker', 'lap_file_scope', 'lap_desktop_')) or n in ('lap_notify_post', 'lap_notify_dismiss')
        if system_call and ret == 'int':
            code += [' if (result == LAP_SYS_EINVAL || result == LAP_SYS_EACCES) fprintf(stderr, "simulator contract: %s %%s (%%d)\\n", result == LAP_SYS_EINVAL ? "EINVAL" : "permission denied", result);' % n]
        if ret == 'int' and n.startswith(('lap_ble_advertise', 'lap_ble_peripheral_', 'lap_gatt_server_', 'lap_gatt_notify', 'lap_hid_device_')):
            code += [' if (result == LAP_IO_EINVAL || result == LAP_IO_EACCES) fprintf(stderr, "simulator contract: %s %%s (%%d)\\n", result == LAP_IO_EINVAL ? "EINVAL" : "permission denied", result);' % n]
        family = 'LAP_SD' if n.startswith('lap_sd_') else 'LAP_NET' if n.startswith(('lap_tcp_', 'lap_net_')) else 'LAP_SD' if n.startswith(('lap_save', 'lap_load', 'lap_settings_', 'lap_file_flush')) else 'LAP_RX'
        if ret == 'int' and n.startswith(('lap_packet_', 'lap_edges_', 'lap_scan_', 'lap_record_', 'lap_replay_', 'lap_channels_', 'lap_io_', 'lap_tx_', 'lap_nfc_', 'lap_sd_', 'lap_tcp_', 'lap_net_', 'lap_rx_', 'lap_radio_', 'lap_audio_', 'lap_event_', 'lap_seen_', 'lap_service_', 'lap_emit', 'lap_gps_', 'lap_motion_', 'lap_navigation_', 'lap_input_', 'lap_caps', 'lap_dialog_', 'lap_text_', 'lap_confirm', 'lap_choose', 'lap_save', 'lap_load', 'lap_settings_', 'lap_file_flush')) and not n.endswith(('dropped', 'source')):
            code += [' if (result == %s_EINVAL || result == %s_EACCES) fprintf(stderr, "simulator contract: %s %%s (%%d)\\n", result == %s_EINVAL ? "EINVAL" : "permission denied", result);' % (family, family, n, family)]
        code += [' ls_lap_sim_call = previous;', ' return result;' if ret != 'void' else '', '}']
        names.append(n)
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    (output / 'lap_contract_wrappers.c').write_text('\n'.join(code)+'\n', encoding='utf-8')
    exported = [n for n in (apidoc.SDK/'exports.txt').read_text().split() if n.startswith(('lap_', 'l3_'))]
    (output / 'lssim_contracts.def').write_text('EXPORTS\n'+'\n'.join(n+'=__wrap_'+n if n in names else n for n in exported)+'\nls_lap_malloc\nls_lap_calloc\nls_lap_realloc\nls_lap_free\n')
    # Windows cmd.exe limits link+post-build commands to 8191 characters.
    # Keep the expanding public API's wrapper flags in GCC's response file.
    response = output / 'lap_contract_link.rsp'
    response.write_text('\n'.join('-Wl,--wrap=' + n for n in names) + '\n', encoding='utf-8')
    (output / 'lap_contract_wrappers.cmake').write_text(
        'target_link_options(lssim PRIVATE "@' + response.resolve().as_posix() + '")\n', encoding='utf-8')


if __name__ == '__main__': generate(sys.argv[1])
