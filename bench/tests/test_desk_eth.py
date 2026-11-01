"""Host only; fake serial/browser, production C framing/queue/budget. No hardware.

python -B bench/tests/test_desk_eth.py
"""
import contextlib
import gzip
import importlib.util
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'host/lsterm'))
import lsdesk
import lsweb


class Launcher(unittest.TestCase):
    def test_options(self):
        self.assertEqual(lsdesk.arguments(['--host', '192.168.1.17']).host, '192.168.1.17')
        self.assertEqual(lsdesk.board_url('192.168.1.17'), 'http://192.168.1.17/')
        for args in (['--host'], ['--host', 'http://a/'], ['--host', '127.0.0.1:80'],
                     ['--host', '999.0.0.1'], ['--host', '1.2.3.4', '--serial']):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                lsdesk.arguments(args)

    def test_explicit_and_remembered_host_never_use_serial(self):
        for explicit, cfg in [('192.168.1.17', {}), (None, {'host': '192.168.1.17'})]:
            with patch.object(lsdesk, 'finder', side_effect=AssertionError('serial accessed')), patch.object(lsdesk, 'save') as save:
                self.assertEqual(lsdesk.desktop_host(cfg, explicit), '192.168.1.17')
                save.assert_called_once_with(cfg)
                self.assertEqual(cfg['host'], '192.168.1.17')

    def test_discovery_and_usb_override(self):
        cfg = {'board': 'fake'}
        with patch.object(lsdesk, 'finder', return_value=lambda: 'FAKE') as find, patch.object(lsdesk, 'ethernet_ip', return_value='10.0.0.17') as ask, patch.object(lsdesk, 'save'):
            self.assertEqual(lsdesk.desktop_host(cfg), '10.0.0.17')
            find.assert_called_once_with(cfg); ask.assert_called_once_with('FAKE')
        with patch.object(lsdesk, 'finder', side_effect=AssertionError('probe')), patch.object(lsdesk, 'save'):
            self.assertIsNone(lsdesk.desktop_host(cfg, serial_only=True))
        self.assertNotIn('host', cfg)

    def test_no_ip_keeps_usb_fallback(self):
        with patch.object(lsdesk, 'finder', return_value=lambda: 'FAKE'), patch.object(lsdesk, 'ethernet_ip', return_value=None), patch.object(lsdesk, 'save') as save:
            self.assertIsNone(lsdesk.desktop_host({}))
            save.assert_not_called()

    def test_main_direct_url_without_bridge(self):
        for invoke in (lambda: lsdesk.main(['--host', '10.0.0.17']), lsweb.main):
            with patch.object(sys, 'argv', ['lsweb.py', '--host', '10.0.0.17']), patch.object(lsdesk, 'load', return_value={}), patch.object(lsdesk, 'save'), patch.object(lsdesk, 'open_window') as opened, patch.object(lsweb, 'serve', side_effect=AssertionError('bridge started')), patch.object(lsdesk, 'finder', side_effect=AssertionError('serial accessed')):
                invoke()
                opened.assert_called_once_with('http://10.0.0.17/')

    def test_console_discovery_only_reads_eth_and_closes(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []; self.closed = False; self.reads = 0
            def open(self):
                self.asserted = self.port == 'FAKE' and self.baudrate == 115200 and not self.dtr and not self.rts
            def write(self, b): self.writes.append(b)
            def read(self, n):
                self.reads += 1
                return b'' if self.reads == 1 else b'eth: on link=up speed=100M ip=10.0.0.17 mask=255.255.255.0 gw=10.0.0.1 dhcp\n'
            def close(self): self.closed = True
        fake = FakeSerial()
        module = types.SimpleNamespace(Serial=lambda: fake, SerialException=OSError)
        with patch.dict(sys.modules, serial=module):
            self.assertEqual(lsdesk.ethernet_ip('FAKE'), '10.0.0.17')
        self.assertTrue(fake.closed and fake.asserted)
        self.assertEqual(fake.writes, [b'\reth\r'])

    def test_config_roundtrip(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'bench') as d, patch.object(lsdesk, 'CONFIG', str(pathlib.Path(d)/'config.json')):
            with patch.object(lsdesk, 'finder', side_effect=AssertionError('serial')):
                lsdesk.desktop_host({'board': 'fake'}, '10.0.0.17')
            self.assertEqual(lsdesk.load(), {'board': 'fake', 'host': '10.0.0.17'})


class Transport(unittest.TestCase):
    def test_no_socket_before_up_addressed_netif(self):
        src = (ROOT/'components/apps/tui/ls_rterm.c').read_text()
        funcs = src[src.index('static esp_netif_t *network_netif('):src.index('/* A connection\'s options:')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef struct { bool up; unsigned ip; } esp_netif_t;
typedef struct { struct { unsigned addr; } ip; } esp_netif_ip_info_t;
static esp_netif_t eth, sta, ap;
static esp_netif_t *def;
static int sockets;
static esp_netif_t *esp_netif_get_default_netif(void) {return def;}
static bool esp_netif_is_netif_up(esp_netif_t *n) {return n->up;}
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *ip) {ip->ip.addr=n->ip;return 0;}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    return !strcmp(key,"ETH_DEF")?&eth:!strcmp(key,"WIFI_STA_DEF")?&sta:&ap;
}
#define ESP_OK 0
#define AF_INET 1
#define SOCK_STREAM 1
#define IPPROTO_TCP 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 1
#define INADDR_ANY 0
#define F_SETFL 1
#define F_GETFL 2
#define O_NONBLOCK 4
struct sockaddr {int unused;};
struct sockaddr_in {int sin_family,sin_port;struct {unsigned s_addr;} sin_addr;};
static int socket(int af,int type,int proto) {(void)af;(void)type;(void)proto;sockets++;return 7;}
static int setsockopt(int fd,int level,int opt,const void *p,size_t n) {
    (void)fd;(void)level;(void)opt;(void)p;(void)n;return 0;
}
static int htons(int p) {return p;}
static unsigned htonl(unsigned p) {return p;}
static int bind(int fd,const struct sockaddr *p,size_t n) {(void)fd;(void)p;(void)n;return 0;}
static int listen(int fd,int backlog) {(void)fd;(void)backlog;return 0;}
static int close(int fd) {(void)fd;return 0;}
static int fcntl(int fd,int cmd,...) {(void)fd;(void)cmd;return 0;}
''' + funcs + r'''
int main(void) {
    assert(tcp_listen(80,4)==-1 && sockets==0); /* no initialized interface */
    def=&eth;eth.ip=123;
    assert(tcp_listen(80,4)==-1 && sockets==0); /* address cached, link down */
    eth.up=true;eth.ip=0;
    assert(tcp_listen(80,4)==-1 && sockets==0); /* DHCP still pending */
    eth.ip=123;def=&sta;
    assert(network_netif()==&eth); /* down default must not hide Ethernet */
    assert(tcp_listen(80,4)==7 && sockets==1);
    eth.up=false;sta.up=true;sta.ip=456;
    assert(network_netif()==&sta && tcp_listen(80,4)==7 && sockets==2);
    sta.up=false;def=NULL;
    assert(tcp_listen(80,4)==-1 && sockets==2);
    puts("socket guard, DHCP wait, Ethernet selection and Wi-Fi fallback: OK");
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'bench') as d:
            c = pathlib.Path(d)/'netif.c'; exe = pathlib.Path(d)/'netif.exe'
            c.write_text(code)
            subprocess.run([shutil.which('gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_production_web_handoff_and_nonblocking_http(self):
        # Compile the real connection/queue/flush/begin functions with fake
        # sockets. This catches queue ordering and takeover reconnect races.
        src = (ROOT/'components/apps/tui/ls_rterm.c').read_text()
        def function(signature):
            start = src.index(signature)
            at = src.index('{', start); depth = 1; end = at + 1
            while depth:
                depth += (src[end] == '{') - (src[end] == '}'); end += 1
            return src[start:end]
        session = src[src.index('typedef struct {'):src.index('} session_t;')+len('} session_t;')]
        at = src.index('typedef struct {\n    int fd;')
        conn = src[at:src.index('} webconn_t;', at)+len('} webconn_t;')]
        prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_vt.h"
#include "ls_rterm_visual.h"
#include "ls_ws.h"
#include "ls_ws_tx.h"
#define MALLOC_CAP_SPIRAM 1
#define WEB_HEAD_US 5000000
#define MSG_DONTWAIT 123
#define SHUT_RDWR 2
#define LS_SNPRINTF(tag, ...) snprintf(__VA_ARGS__)
typedef struct { int kind; } ls_input_ev_t;
#define LS_IEV_RELEASE_ALL 1
static int releases, updates, blocked, send_calls;
static int64_t now;
static char sent[4][20000];
static size_t sent_n[4];
static int send(int fd,const void *data,size_t n,int flags) {
    assert(fd>=0 && fd<4 && flags==MSG_DONTWAIT);send_calls++;
    if(blocked) {errno=EAGAIN;return -1;}
    if(n>7)n=7;
    memcpy(sent[fd]+sent_n[fd],data,n);sent_n[fd]+=n;return (int)n;
}
static int shutdown(int fd,int how) {(void)fd;assert(how==SHUT_RDWR);return 0;}
static int close(int fd) {(void)fd;return 0;}
static int64_t esp_timer_get_time(void) {return now;}
static void *heap_caps_realloc(void *p,size_t n,int caps) {assert(caps==1);return realloc(p,n);}
static void heap_caps_free(void *p) {free(p);}
static void ls_input_post(const ls_input_ev_t *e) {assert(e->kind==1);releases++;}
static void ls_tui_geometry(int *cols,int *rows,void *cw,void *ch) {
    (void)cw;(void)ch;*cols=150;*rows=39;
}
static void update_want(void) {updates++;}
''' + session + conn + r'''
static session_t s_web;
static webconn_t s_wc[3];
static int s_web_viewer=-1;
static uint32_t s_web_ip;
static bool s_input_new;
static bool session_buffers(session_t *s) {(void)s;return true;}
'''
        funcs = '\n'.join(function(x) for x in (
            'static void wc_close(int i)\n{', 'static void *web_resize(',
            'static bool web_queue(', 'static bool ws_send(', 'static int web_send_now(',
            'static void web_flush(', 'static void ws_say(int i, const char *text) {',
            'static void web_write(', 'static void web_begin('))
        main = r'''
int main(void) {
    for(int i=0;i<3;i++)s_wc[i].fd=-1;
    s_wc[0].fd=0;s_wc[1].fd=1;
    web_begin(0,"ack z cells",now);
    assert(s_web.on && s_web.cells && s_web.z && s_web.acks);
    web_flush(0,now);assert(s_wc[0].fd==0 && !ls_ws_tx_pending(&s_wc[0].tx));
    assert(ws_send(0,LS_WS_BINARY,"old delta",9));
    blocked=1;send_calls=0;web_flush(0,now);
    assert(send_calls==1 && ls_ws_tx_pending(&s_wc[0].tx));
    web_begin(1,"ack z cells",now);
    assert(s_web_viewer==1 && s_web.on && s_wc[0].closing && s_wc[0].fd==0);
    assert(releases==1); /* switching viewers releases held keys */
    blocked=0;web_flush(0,now);
    assert(s_wc[0].fd==-1 && s_web_viewer==1 && s_web.on);
    /* Taken is sent before close, preventing old-page reconnect ping-pong. */
    assert(sent_n[0]>24);
    assert(!memcmp(sent[0]+sent_n[0]-15,"term: web taken",15));
    /* Header finishes before a flash body; slow HTTP does not loop/wait. */
    s_wc[2].fd=2;
    assert(web_queue(2,"HEAD",4));
    s_wc[2].body=(const uint8_t *)"FLASH BODY";s_wc[2].body_n=10;s_wc[2].closing=true;
    blocked=1;send_calls=0;web_flush(2,now);
    assert(send_calls==1 && sent_n[2]==0 && s_wc[2].body_n==10);
    blocked=0;web_flush(2,now);web_flush(2,now);
    assert(s_wc[2].fd==-1 && sent_n[2]==14 && !memcmp(sent[2],"HEADFLASH BODY",14));
    /* No progress disconnects a client even if it keeps sending pings. */
    blocked=1;now=WEB_HEAD_US+1;web_flush(1,now);
    assert(s_wc[1].fd==-1 && !s_web.on && s_web_viewer==-1);
    puts("production web takeover, partial sends, HTTP body ordering and stall timeout: OK");
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'bench') as d:
            c = pathlib.Path(d)/'web.c'; exe = pathlib.Path(d)/'web.exe'
            c.write_text(prefix + funcs + main)
            subprocess.run([shutil.which('gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-variable', '-I'+str(ROOT/'components/apps/tui'),
                            str(c), str(ROOT/'components/apps/tui/ls_ws.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_production_c(self):
        gcc = shutil.which('gcc')
        self.assertIsNotNone(gcc, 'gcc required')
        with tempfile.TemporaryDirectory(dir=ROOT/'bench') as d:
            exe = pathlib.Path(d)/'desktop.exe'
            subprocess.run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'components/apps/tui'),
                            str(ROOT/'bench/tests/test_ws_desktop.c'), str(ROOT/'components/apps/tui/ls_ws.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_embedded_page_and_glyph_gzip(self):
        web = ROOT/'components/apps/tui/web'
        spec = importlib.util.spec_from_file_location('web_pack', web/'pack.py')
        pack = importlib.util.module_from_spec(spec); spec.loader.exec_module(pack)
        with tempfile.TemporaryDirectory(dir=ROOT/'bench') as d:
            out = pathlib.Path(d)/'page.gz'; pack.main(str(web/'screen.html'), str(out))
            html = gzip.decompress(out.read_bytes()).decode()
        self.assertNotIn('<script src=', html)
        self.assertIn("fetch('lsfont.bin.gz')", html)
        self.assertIn('term caps', html); self.assertIn('term visual', html)
        self.assertIn("say('term ack')", html)
        self.assertEqual(gzip.decompress((web/'lsfont.bin.gz').read_bytes())[:4], b'LSF2')
        for name in ('dejavu-mono.woff2', 'dejavu-mono-bold.woff2'):
            self.assertEqual((web/name).read_bytes()[:4], b'wOF2')


if __name__ == '__main__':
    unittest.main()
