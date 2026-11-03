"""API policy metadata derived from declarations/guards and the export allowlist.

No second list of exported names. Family policies describe host backends, not
hardware support. Unknown exports are explicitly missing, never silently real.
"""
import re
from pathlib import Path


def version(sdk):
    text = (sdk / 'include/lakeshark_app.h').read_text(encoding='utf-8')
    pairs = re.findall(r'#define LAP_API_MAJOR (\d+)\s*\n#define LAP_API_MINOR (\d+)', text)
    return max(tuple(map(int, pair)) for pair in pairs)


def since(entry, sdk):
    text = (sdk / 'include' / entry['header']).read_text(encoding='utf-8')
    # A documentation cross-reference can precede the declaration's version
    # guard (for example the 2.1 service docs mention the new 2.6 limit call).
    clean = re.sub(r'/\*.*?\*/|//[^\n]*',lambda m:' '*len(m[0]),text,flags=re.S)
    before = text[:re.search(r'\b' + entry['name'] + r'\s*\(', clean).start()]
    guards = []
    for line in before.splitlines():
        if re.match(r'\s*#\s*if', line): guards.append(line)
        elif re.match(r'\s*#\s*endif', line) and guards: guards.pop()
    for minor in (6, 5, 4, 3, 2, 1):
        if any('LAP_API_2' + str(minor) in g for g in guards): return '2.' + str(minor)
    n = entry['name']
    if entry['header'] == 'lakeshark_ui.h': return '2.1'
    if entry['header'] == 'lap3d.h':
        m = re.search(r'API (\d+\.\d+)', text[:1000])
        return m.group(1) if m else 'unknown'
    if any('!defined(LAP_TARGET_API_1)' in g for g in guards): return '2.0'
    if n.startswith(('lap_tcp_', 'lap_net_')): return '1.3'
    if n.startswith(('lap_aircraft', 'lap_say')) or n == 'lap_immersive': return '1.2'
    marker = text.find('API 1.1: the board itself')
    if marker >= 0 and len(before) > marker: return '1.1'
    return '1.0'


def policy(n):
    cap = None
    if n.startswith(('lap_radio_', 'lap_rx_', 'lap_audio_listen', 'lap_audio_read', 'lap_event_', 'lap_seen_', 'lap_emit')): cap = 'LAP_CAP_RADIO_RX'
    if n.startswith(('lap_tcp_', 'lap_net_')): cap = 'LAP_CAP_NETWORK' if n != 'lap_net_status' else None
    if n in ('lap_mesh_send', 'lap_mesh_dm', 'lap_mesh_advert'): cap = 'LAP_CAP_RADIO_TX'
    if n == 'lap_service_start': cap = 'LAP_CAP_SERVICE'
    if n in ('lap_event_dropped', 'lap_event_source', 'lap_emit_dropped', 'lap_seen_dropped'): cap = None
    status, reason = 'real', 'Shared app runtime and host renderer; target ABI/timing still require board validation.'
    if n.startswith(('lap_packet_', 'lap_edges_', 'lap_scan_', 'lap_record_', 'lap_replay_', 'lap_channels_', 'lap_io_', 'lap_tx_', 'lap_nfc_')) or n == 'lap_radio_modes':
        cap = 'LAP_CAP_RADIO_TX' if n.startswith('lap_tx_') else 'LAP_CAP_RADIO_RX'
        if n.startswith('lap_nfc_') or n in ('lap_radio_modes', 'lap_io_status'): cap = None
        status, reason = 'limited', 'Shared bounded radio-I/O queues, framed SD files and replay; scripted synthetic radio/NFC backend, no RF. See RADIO_IO.md.'
    elif n.startswith(('lap_radio_', 'lap_rx_')):
        status, reason = 'faked', 'Shared radio/DSP/typed decoder workers with synthetic SIM I/Q; rxdelay/rxresult/decode fixtures via LAPSIM_SCRIPT, exact range holes and hot unplug; RF timing requires board validation.'
    elif n.startswith(('lap_motion', 'lap_gps', 'lap_battery', 'lap_time', 'lap_touch', 'lap_swipe', 'lap_aircraft')):
        status, reason = 'faked', 'Host snapshots or scripted synthetic input; no hardware acquisition.'
    elif n.startswith(('lap_mesh_', 'lap_wifi_', 'lap_ble_')):
        status, reason = 'limited', 'Host snapshots/stubs; no wireless backend, real scan lease or mesh delivery.'
    elif n.startswith(('lap_sound', 'lap_say', 'lap_tone', 'lap_haptic', 'lap_vibrate', 'lap_audio_')):
        status, reason = 'limited', 'Queue/count/log behavior only; no speaker, receiver PCM or haptic hardware; host drains immediately.'
    elif n in ('lap_put_glass', 'lap_cell_px', 'lap_palette'):
        status, reason = 'limited', 'Host theme/cell metrics fallback; glass text is rendered as ordinary cells.'
    elif n.startswith(('lap_fullscreen', 'lap_immersive', 'lap_hold_rotation', 'lap_keep_awake', 'lap_brightness')):
        status, reason = 'limited', 'Host display/power stubs; immersive only with LSSIM_IMMERSIVE=1.'
    elif n.startswith(('lap_tcp_', 'lap_net_')):
        status, reason = 'limited', 'Real nonblocking host IPv4 sockets and worker lifecycle; host DNS/Wi-Fi and wall-clock timing differ.'
    elif n.startswith('lap_sd_'):
        status, reason = 'limited', 'Shared bounded jobs on a host directory; FAT latency/power-loss behavior is not simulated.'
    elif n.startswith(('lap_service_', 'lap_screen_open')):
        status, reason = 'limited', 'Shared per-instance hide/reopen/stop, resource tables and tick budget. LAPSIM_SERVICES=service_logger,service_notifier runs two residents with a foreground app; service_stop=ID stops one. Device scheduling and physical leases require board checks.'
    elif n.startswith(('lap_action', 'lap_notify', 'lap_alert', 'lap_seen_')):
        status, reason = 'limited', 'Shared validation/queues; host action, notice or watcher producers differ from device services.'
    elif n in ('lap_caps',):
        status, reason = 'limited', 'Shared capability query; LAPSIM_HW configures Wi-Fi/BLE/GPS/etc. and LAPSIM_RADIO=none removes SIM radio; no physical hardware.'
    elif n.startswith(('lap_save', 'lap_load', 'lap_settings_', 'lap_file_flush')):
        status, reason = 'limited', 'Shared durable-data/cache worker over host files; FAT power-loss behavior and SD timing need board tests.'
    elif n in ('lap_text_input', 'lap_confirm', 'lap_choose', 'lap_dialog_poll', 'lap_dialog_cancel'):
        status, reason = 'limited', 'Shared modal validation/completion/cancel; LAPSIM_SCRIPT dialogtext/dialogpick/dialogcancel supplies answers; trusted device UI still needs board validation.'
    elif n.startswith(('lap_input', 'lap_key_', 'lap_pointer', 'lap_system_', 'lap_text_')):
        status, reason = 'limited', 'Shared held-key/state/event queue fed by host keys/mouse and LAPSIM_SCRIPT key edges, buttons, wheel, focus and resize; host geometry/timing differ.'
    if n.startswith(('lap_http_', 'lap_udp_', 'lap_mdns_', 'lap_operation_', 'lap_time_sync')) or n in ('lap_time_status', 'lap_wifi_status', 'lap_wifi_scan_start', 'lap_wifi_scan_result', 'lap_wifi_connect', 'lap_wifi_disconnect', 'lap_wifi_ap_request'):
        cap = None if n == 'lap_time_status' else 'LAP_CAP_NETWORK'
        status, reason = 'limited', 'Shared bounded services; Wi-Fi/HTTPS/DNS-SD/time fixtures and real UDP host sockets. LAPSIM_SCRIPT wifierror/httperror/httpbody/wifidown inject outcomes; device TLS, radio and credential persistence require board tests.'
    if n.startswith('lap_rx_dsp_'):
        status, reason = 'limited', 'Copied channel blocks and private native worker; host thread deadline containment, two input blocks, eight results, drop/overrun accounting. LAPSIM_SCRIPT dspcost=US injects missed budgets and dspfail=1 injects memory admission failure; device FreeRTOS preemption and watchdog need board verification.'
    if n.startswith(('lap_ble_scan_', 'lap_ble_advert_', 'lap_ble_connect', 'lap_ble_disconnect', 'lap_ble_event_', 'lap_gatt_')):
        cap = None
        status, reason = 'limited', 'Shared lease, copied advertisement and GATT queues with a synthetic peripheral. Board uses existing NimBLE host; pairing, coexistence and controller RAM need hardware tests.'
    if n.startswith(('lap_mixer_', 'lap_mic_', 'lap_sample_', 'lap_audio_record_', 'lap_audio_operation', 'lap_audio_close')):
        cap = 'LAP_CAP_MIC' if n.startswith(('lap_mic_', 'lap_audio_record_')) else None
        status, reason = 'limited', 'Shared mixer, PCM queues and WAV file parsing/recording; synthetic microphone; no host speaker. Hardware codec arbitration, mic indicator and real-time timing require board tests.'
    if n.startswith(('lap_ipc_', 'lap_launch_', 'lap_stream_', 'lap_storage_', 'lap_dir_', 'lap_file_picker', 'lap_file_scope', 'lap_desktop_')) or n in ('lap_notify_post', 'lap_notify_dismiss'):
        cap = None
        status, reason = 'limited', 'Shared copied notice/launch broker, bounded topic mailboxes and per-instance storage/UI state; API 2.6 subscriptions/notices expire on unload while 2.5 persistence is retained. LAPSIM_SCRIPT desktop/pick/pickcancel/notice fixtures. FAT durability, actual viewer lifecycle and trusted launcher need board checks. PC URL/save requests negotiate desk1; bounded copied files and scripted completions; real OS dialogs/USB/RF need hardware verification.'
    if n.startswith(('lap_gps_', 'lap_motion_', 'lap_navigation_', 'lap_input_next2', 'lap_input_devices', 'lap_input_device_state')):
        cap = None
        status, reason = 'limited', 'Shared focused input queue, bounded GPS lease/motion delivery/navigation snapshots; LAPSIM_SCRIPT device/gamepad/contact/gpsfix/satellite/motionquality fixtures and host track/place files. BLE HID, controller IDs, UART and sensor accuracy require board tests. See INPUT_SENSORS.md.'
    if n.startswith('lap_desktop_'):
        status, reason = 'limited', 'Shared desk1 broker; desktopcaps=FLAGS,SCALE and desktopresult=RESULT fixtures exercise bounded export, completion, cancellation and DPI. Old viewers return unsupported; OS dialogs need a real viewer.'
    if n == 'lap_tx_hackrf_config':
        cap = 'LAP_CAP_RADIO_TX'
        status, reason = 'limited', 'Shared copied TX settings; hackrf/hackrflimits/hackrfrx/hackrftemp fixtures. Synthetic USB output; physical RF and disconnect timing require a board.'

    # API 2.6: Bluetooth peripheral roles.
    if n.startswith(('lap_ble_advertise', 'lap_ble_peripheral_', 'lap_gatt_server_', 'lap_gatt_notify', 'lap_hid_device_')):
        cap = 'LAP_CAP_RADIO_TX | LAP_CAP_HID_DEVICE' if n.startswith('lap_hid_device_') and n != 'lap_hid_device_report_map' else 'LAP_CAP_RADIO_TX'
        if n in ('lap_ble_peripheral_status', 'lap_gatt_server_next', 'lap_hid_device_report_map', 'lap_ble_advertise_stop', 'lap_gatt_server_close', 'lap_hid_device_close'): cap = None
        status, reason = 'limited', 'Shared bounded peripheral database, copied ATT values, event and send queues. LAPSIM_SCRIPT periphconnect/periphdisconnect/periphread/periphwrite/periphsubscribe/periphmtu/periphbond/periphbusy/peripherror/periphack simulate peers. No RF; controller power, HOGP pairing and internal RAM need board verification. See BLE_PERIPHERAL.md.'
    if n == 'lap_text_size':
        status, reason = 'real', 'Shared renderer text measurement.'
    if n in ('lap_random', 'lap_millis', 'lap_frame'):
        status, reason = 'faked', 'Deterministic host random/frame/virtual clock source; not hardware entropy or real-time scheduling.'
    if n.startswith('lap_value_'):
        status, reason = 'limited', 'Shared value lookup over the host registry; device-only producers may be absent.'
    if n == 'lap_action': cap = 'action-dependent (RX/TX/service policy)'
    return cap, {'status': status, 'reason': reason}


def enrich(entries, sdk):
    current = version(sdk)
    exports = {n.strip() for n in (sdk / 'exports.txt').read_text().splitlines() if n.strip() and not n.lstrip().startswith('#')}
    out = []
    for e in entries:
        e = dict(e)
        # API 2.5 games/assets: software helpers share board/simulator code.
        if e.get('library') and e['header'] in ('lap_assets.h','lap_game.h','lap_music.h'):
            e['sdk_since'] = '2.5'
            e['simulator'] = dict(status='real', reason='Same app-compiled code on board and Studio; SD and mixer imports retain their existing simulated contracts. See GAMES_ASSETS.md.')
        if e['name'] in ('LAP_API_MAJOR', 'LAP_API_MINOR'):
            e['signature'] = e['name'] + '  ' + str(current[0 if e['name'].endswith('MAJOR') else 1])
            e['doc'] = 'Default SDK target; generated from the version selection in lakeshark_app.h.'
        if e['kind'] == 'function' and e['name'] in exports:
            e['since'] = since(e, sdk)
            e['capability'], e['simulator'] = policy(e['name'])
            e['exported'] = True
            e['pointer_contract'] = (
                'Destination retained until lap_sd_poll completes; app image or live app allocation required. Path copied synchronously.'
                if e['name'] in ('lap_sd_read', 'lap_sd_list') else
                'Service struct copied synchronously; retained callback code must be in the app image.'
                if e['name'] == 'lap_service_start' else
                'Widget data copied synchronously; retained callback code must be in the app image.'
                if e['name'] in ('lap_options_open', 'lap_picker_open', 'lap_numpad_open') else
                'Arrays retained through l3_end; mesh/terrain/walls/particles arrays must survive until the next l3_begin if raycast is used. Use app-owned storage for retained data.'
                if e['name'] in ('l3_draw_mesh', 'l3_terrain', 'l3_walls', 'l3_particles', 'l3_sprite', 'l3_label') else
                'Synchronous input/output buffers may use the active callback stack; opaque scene handles follow the lap3d header lifetime.'
                if e['name'].startswith('l3_') else
                'Synchronous input/output buffers may use the active callback stack; runtime retains no caller data buffer.')
            e['doc'] += ' Pointer contract: ' + e['pointer_contract']
            e['doc'] += ' Since API %s. Capability: %s. Simulator: %s — %s' % (
                e['since'], e['capability'] or 'none', e['simulator']['status'], e['simulator']['reason'])
        out.append(e)
    known = {e['name'] for e in out}
    for n in sorted(exports - known):
        if n.startswith(('lap_', 'l3_')):
            out.append(dict(name=n, kind='function', signature='', doc='Export has no SDK declaration.', header='',
                            exported=True, since='unknown', capability=None,
                            simulator=dict(status='missing', reason='No parsed SDK declaration; registry generation must be fixed.')))
    return out


def generate(sdk, entries):
    import json
    calls = sorted((e for e in entries if e.get('exported')), key=lambda e: e['name'])
    libraries = sorted((e for e in entries if e.get('library') and e.get('simulator') and e['kind']=='function'), key=lambda e:e['name'])
    data = dict(schema=1, api_version='.'.join(map(str, version(sdk))), calls=calls, libraries=libraries)
    docs = sdk / 'docs'; docs.mkdir(exist_ok=True)
    (docs / 'api.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    (docs / 'simulator.json').write_text(json.dumps([dict(name=e['name'], **e['simulator']) for e in calls], indent=2) + '\n', encoding='utf-8')
    table = '# Simulator coverage (generated by `python app-sdk/apidoc.py --generate`)\n\n'
    table += 'API %s. Synchronous inputs and outputs may use the active callback stack; retained SD read/list destinations require app image or tracked app allocations until poll completion.\n\n' % data['api_version']
    table += ('Host controls: `LAPSIM_HW=wifi,ble,gps,imu,sd,desktop` sets the exact hardware inventory (default `wifi,ble,imu,sd,desktop`; `none` clears it). '
              '`LAPSIM_RADIO=none|sim` controls radio presence. API 2.4 adds `rxdelay=N` worker polls, `rxresult=-9` next control failure, and `decode=page,RIC,text` (also flex/aprs/acars/ais/adsb/p25/same); fixtures only reach explicitly attached matching decoders and carry SYNTHETIC provenance. Script steps use `N:command` separated by semicolons: '
              '`keydown=W`, `keyup=W`, `keydown=@left`, `buttondown=x,y,button[,buttons]`, `buttonup=x,y,button[,buttons]`, '
              '`pointer=x,y,buttons`, `wheel=-3`, `focus=0|1`, `resize=w,h`, `caps=wifi,sd`, '
              '`dialogtext=answer`, `dialogpick=0`, `dialogcancel`. `key=` remains a down/up pulse. '
              'Mouse buttons are 0=left, 1=middle, 2=right. Save/settings and cached files live under `sdcard/apps_data/<app-id>/`, '
              'using the firmware staging, backup, replacement and recovery path. Resize injects an event; renderer geometry remains authoritative.\n\n')
    table += ('API 2.4 fixtures: `wifierror=<negative code>`, `httperror=<negative code>`, `httpstatus=200`, `httpbody=text`, `wifidown`, '
              '`udpdrop=0|1`, `mdnserror=<negative code>`, `mdnsempty=0|1`, `blesilence=0|1`, `blead=RSSI,HEX`, '
              '`gatterror=<negative code>`, `blenotify=HEX`, `bledisconnect`, `micpcm=amplitude`, `micwav=canonical.wav`, '
              '`mixpcm=output.s16le`, `audiomute=0|1`, `audiolive=0|1`. Add `speaker,mic` to LAPSIM_HW to fit simulated audio hardware. '
              'The host defaults to SIM WLAN, deterministic verified-HTTPS responses and a battery-service peripheral. No external HTTPS connection is made. '
              'See [network/audio contracts](NET_AUDIO.md) for bounds and device checks.\n\n')
    table += ('API 2.6 Bluetooth peripheral fixtures: `LAPSIM_PERIPHERAL=none|advertiser|gatt|hid` selects build features. '
              '`periphbusy=0|1`, `peripherror=<code>`, `periphconnect`, `periphdisconnect`, `periphmtu=185`, '
              '`periphbond=0|1`, `periphsubscribe=ID,BITS`, `periphread=ID`, `periphwrite=ID,HEX`, '
              '`periphack=0|1`. See [BLE peripheral contracts and RAM](BLE_PERIPHERAL.md).\n\n')
    table += ('API 2.5 system fixtures: `desktop=0|1`, `notice=ID,ACTION`, `noticeaction=-1|0|1` (latest app notice), '
              '`pick=INDEX`, `pickcancel`; picker rows 0..3 are cancel/up/use-folder/next-page and data begins at 4. '
              'Use `sdcard/shared/` for public file fixtures; `caps=none` injects card removal. '
              'See [system service contracts](SYSTEM_SERVICES.md).\n\n')
    table += 'Real = shared software; faked = synthetic source; limited = host differences; missing = unavailable.\n\n'
    table += '| Call | Since API | Capability | Simulator | Reason |\n|---|---|---|---|---|\n'
    for e in calls:
        table += '| `%s` | %s | %s | %s | %s |\n' % (e['name'], e['since'], e['capability'] or 'none', e['simulator']['status'], e['simulator']['reason'])
    table += '\n## SDK 2.5 games/assets libraries (no firmware exports)\n\n'
    table += 'PNG/mesh fixtures are ordinary host SD files; missing/corrupt/oversized files follow the same loader as the board. `LAPSIM_SCRIPT` supplies held keys, touch/mouse, focus, resize, `mixpcm` and `audiomute` through existing services.\n\n'
    table += '| Helper | Library | Simulator | Reason |\n|---|---|---|---|\n'
    for e in libraries:
        table += '| `%s` | `%s` | %s | %s |\n' % (e['name'],e['header'],e['simulator']['status'],e['simulator']['reason'])
    (docs / 'SIMULATOR.md').write_text(table, encoding='utf-8')
    path = sdk / 'README.md'; text = path.read_text(encoding='utf-8')
    block = '<!-- generated-api:start -->\nCurrent default target: **API %s**. Exported SDK calls: **%d**.\nSee the [generated API registry](docs/api.json) and [simulator coverage](docs/SIMULATOR.md).\n<!-- generated-api:end -->' % (data['api_version'], len(calls))
    if '<!-- generated-api:start -->' in text:
        text = re.sub(r'<!-- generated-api:start -->.*?<!-- generated-api:end -->', lambda _: block, text, flags=re.S)
    else: text = text.replace('\n', '\n\n' + block + '\n', 1)
    path.write_text(text, encoding='utf-8')
