/* BLE EXPLORER flows (app-sdk/apps/ble_explorer/bx_flow.c): the parser on
   good and bad lines, the built-in list (the same text as
   flows/ble-flows.txt, every flow good), matching and order, and runs
   driven a step a frame against a made-up device the way the board reports
   events: reads, writes, notifications, waits, a lost reply given up by
   bx_overdue, an error stop, optional steps, service-scoped UUIDs. Also the
   WeChat AirSync notification decode. */
#include "ls_test.h"
#include "bx_flow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bxf_lib_t lib;
static bxf_flow_t fl;

static int parse(const char *t, bxf_err_t *err) { return bxf_parse(t, strlen(t), 1, &fl, NULL, err); }

LS_CASE(flow_parser_good_lines) {
    bxf_err_t err = {0};
    const char *t =
        "flow Thermal printer: status   # a comment\n"
        "about Asks a common BLE thermal printer for its status\n"
        "match service 18f0\n"
        "match name *Printer*\n"
        "\n"
        "  # an indented comment\n"
        "notify 2af0 on\n"
        "write 2af1 10 04 01          # hex bytes\n"
        "write 0x2AF1 \"AT#1\\r\\n\" 0d0a\n"
        "? read 1800/2a00\n"
        "wait notify 2af0 2000\n"
        "wait 250\n"
        "log done here\n"
        "write 6e400002-b5a3-f393-e0a9-e50e24dcca9e \"\\x41\\\"\"\n"
        "write 00002a2b-0000-1000-8000-00805f9b34fb $time\n";
    LS_EQ_INT(parse(t, &err), 0);
    LS_EQ_STR(fl.name, "Thermal printer: status");
    LS_EQ_STR(fl.about, "Asks a common BLE thermal printer for its status");
    LS_EQ_INT(fl.nmatch, 2);
    LS_EQ_INT(fl.match[0].kind, BXF_M_SERVICE);
    LS_EQ_UINT(bx_uuid16(&fl.match[0].uuid), 0x18F0u);
    LS_EQ_STR(fl.match[1].glob, "*Printer*");
    LS_EQ_INT(fl.nsteps, 9);
    LS_CHECK(fl.writes);
    LS_EQ_INT(fl.step[0].op, BXF_NOTIFY);
    LS_CHECK(fl.step[0].on);
    LS_EQ_INT(fl.step[1].op, BXF_WRITE);
    LS_EQ_INT(fl.step[1].n, 3);
    LS_CHECK(!memcmp(fl.step[1].data, "\x10\x04\x01", 3));
    LS_EQ_INT(fl.step[1].srclen, (int)strlen("write 2af1 10 04 01"));     /* the comment is not the step's */
    LS_EQ_INT(fl.step[2].n, 8);                                            /* "AT#1\r\n" then 0d 0a: a # in quotes stays */
    LS_CHECK(!memcmp(fl.step[2].data, "AT#1\r\n\r\n", 8));
    LS_EQ_UINT(bx_uuid16(&fl.step[2].uuid), 0x2AF1u);
    LS_CHECK(fl.step[3].optional && fl.step[3].in_svc);
    LS_EQ_UINT(bx_uuid16(&fl.step[3].svc), 0x1800u);
    LS_EQ_UINT(bx_uuid16(&fl.step[3].uuid), 0x2A00u);
    LS_EQ_INT(fl.step[4].op, BXF_WAIT_NOTIFY);
    LS_EQ_UINT(fl.step[4].ms, 2000u);
    LS_EQ_INT(fl.step[5].op, BXF_WAIT);
    LS_EQ_INT(fl.step[6].op, BXF_LOG);
    LS_EQ_INT(fl.step[7].uuid.len, 16);
    LS_EQ_INT(fl.step[7].n, 2);
    LS_CHECK(!memcmp(fl.step[7].data, "A\"", 2));
    /* a SIG-base 128-bit UUID comes back short; $time is the 10 bytes of 2a2b */
    LS_EQ_INT(fl.step[8].uuid.len, 2);
    LS_EQ_INT(fl.step[8].n, 10);
    LS_CHECK(fl.step[8].time);
    const bxf_time_t tm = {2026, 11, 26, 14, 5, 9, 4};
    LS_EQ_INT(bxf_parse(t, strlen(t), 1, &fl, &tm, &err), 0);
    LS_CHECK(!memcmp(fl.step[8].data, "\xea\x07\x0b\x1a\x0e\x05\x09\x04\x00\x01", 10));
}

LS_CASE(flow_parser_bad_lines) {
    static const struct { const char *text; int line; const char *msg; } bad[] = {
        {"flow A\nwirte 2a00 01\n", 2, "unknown step 'wirte'"},
        {"flow A\nwrite 2a00\n", 2, "write needs data"},
        {"flow A\nwrite 2a00 0\n", 2, "hex comes in pairs"},
        {"flow A\nwrite 2a00 zz\n", 2, "bad hex"},
        {"flow A\nwrite 2a00 \"open\n", 2, "no closing quote"},
        {"flow A\nread 2a0\n", 2, "bad UUID '2a0'"},
        {"flow A\nread\n", 2, "a UUID is missing"},
        {"flow A\nnotify 2a37 maybe\n", 2, "notify needs on or off"},
        {"flow A\nwait 99999\n", 2, "a wait is 30000 ms at most"},
        {"flow A\nwait soon\n", 2, "wait needs a time in ms"},
        {"flow A\nwait notify 2a37\n", 2, "wait needs a time in ms"},
        {"flow A\nread 2a00 2a01\n", 2, "too many words"},
        {"flow A\nmatch colour red\nread 2a00\n", 2, "match service or match name"},
        {"flow A\nabout nothing\n", 1, "no steps"},
        {"flow\nread 2a00\n", 1, "flow needs a name"},
        {"read 2a00\n", 1, "'read' before the flow line"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bxf_err_t err = {0};
        LS_EQ_INT(parse(bad[i].text, &err), -1);
        LS_EQ_INT(err.line, bad[i].line);
        LS_EQ_STR(err.msg, bad[i].msg);
    }
    /* one byte more than a write holds */
    char many[BX_VAL * 3 + 32] = "flow A\nwrite 2a00";
    for (int i = 0; i <= BX_VAL; i++) strcat(many, " 5a");
    bxf_err_t e2 = {0};
    LS_EQ_INT(parse(many, &e2), -1);
    LS_EQ_STR(e2.msg, "too many bytes for one write");
    many[strlen(many) - 3] = 0;
    LS_EQ_INT(parse(many, &e2), 0);
    LS_EQ_INT(fl.step[0].n, BX_VAL);
    /* 33 steps is one too many */
    char big[1200] = "flow Long\n";
    for (int i = 0; i < 33; i++) strcat(big, "read 2a00\n");
    bxf_err_t err = {0};
    LS_EQ_INT(parse(big, &err), -1);
    LS_EQ_STR(err.msg, "more than 32 steps");
    LS_EQ_INT(err.line, 34);
}

LS_CASE(flow_builtins_match_the_pack_source_and_all_parse) {
    FILE *f = fopen(BX_FLOWS_TXT, "rb");
    LS_CHECK(f != NULL);
    if (!f) return;
    static char txt[65536];
    const size_t n = fread(txt, 1, sizeof(txt), f);
    fclose(f);
    LS_EQ_UINT(n, bx_flows_builtin_len);
    LS_CHECK(!memcmp(txt, bx_flows_builtin, n));
    bxf_lib_init(&lib);
    const int got = bxf_lib_add(&lib, bx_flows_builtin, bx_flows_builtin_len, BXF_ORIGIN_BUILTIN, "built-in");
    LS_EQ_INT(lib.bad, 0);
    if (lib.bad) printf("bad: %s:%d %s\n", lib.err.where, lib.err.line, lib.err.msg);
    LS_CHECK(got >= 30);
    LS_EQ_INT(lib.n, got);
    for (int i = 0; i < lib.n; i++) {
        const bxf_entry_t *e = &lib.e[i];
        LS_CHECK(e->about[0] != 0);                     /* every flow says what it does */
        LS_CHECK(e->nmatch > 0);                        /* and which devices it is for */
        bxf_err_t err;
        LS_EQ_INT(bxf_parse(e->text, e->len, e->line0, &fl, NULL, &err), 0);
        LS_CHECK(!fl.writes || fl.confirm[0] || strstr(fl.name, "status") || strstr(fl.name, "ping") || strstr(fl.name, "info"));
        for (int s = 0; s < fl.nsteps; s++) LS_CHECK(fl.step[s].op != BXF_WAIT || fl.step[s].ms <= 10000u);
    }
}

LS_CASE(flow_sources_replace_by_name_in_load_order) {
    bxf_lib_init(&lib);
    static const char builtin[] = "flow Battery level\nabout old\nmatch service 180f\nread 2a19\n"
                                  "flow Find me\nabout beep\nmatch service 1802\nwrite 2a06 02\n";
    static const char pack[] = "# the pack\nflow battery LEVEL\nabout newer\nmatch service 180f\nread 2a19\nread 2a19\n";
    static const char user[] = "flow Mine\nabout my lamp\nmatch name Lamp*\nwrite fff3 01\n"
                               "flow Broken\nabout x\nmatch name x\nfrob 1\n";
    LS_EQ_INT(bxf_lib_add(&lib, builtin, strlen(builtin), BXF_ORIGIN_BUILTIN, "built-in"), 2);
    LS_EQ_INT(bxf_lib_add(&lib, pack, strlen(pack), BXF_ORIGIN_PACK, "ble-flows.bin"), 1);
    LS_EQ_INT(bxf_lib_add(&lib, user, strlen(user), BXF_ORIGIN_USER, "lamp.flow"), 1);
    LS_EQ_INT(lib.n, 3);
    LS_EQ_INT(lib.replaced, 1);
    LS_EQ_STR(lib.e[0].about, "newer");                 /* replaced in place */
    LS_EQ_INT(lib.e[0].origin, BXF_ORIGIN_PACK);
    LS_EQ_INT(lib.e[0].nsteps, 2);
    LS_EQ_INT(lib.bad, 1);
    LS_EQ_STR(lib.err.where, "lamp.flow");
    LS_EQ_INT(lib.err.line, 8);
    LS_EQ_STR(lib.err.msg, "unknown step 'frob'");
    /* the ones that fit come first */
    bx_conn_t *c = calloc(1, sizeof(*c));
    c->nsvc = 1;
    c->svc[0].uuid = (bx_uuid_t){{0x02, 0x18}, 2};
    int order[BXF_FLOWS];
    LS_EQ_INT(bxf_order(&lib, c, "Lamp 7", order), 2);
    LS_EQ_INT(order[0], 1);
    LS_EQ_INT(order[1], 2);
    LS_EQ_INT(order[2], 0);
    LS_CHECK(bxf_glob("*printer*", "MPT-II Printer"));
    LS_CHECK(bxf_glob("GB0?", "gb01"));
    LS_CHECK(!bxf_glob("GB0?", "GB012"));
    free(c);
}

/* ---- running against a made-up device ---- */
static bx_conn_t *dev;
static uint32_t now;
static int sent_reads, sent_writes, sent_subs;
static uint8_t last_write[BX_VAL], last_wn;
static bool last_nr;

static void add_chr(uint16_t svc_i, uint16_t value, uint8_t props, bx_uuid_t u, uint16_t cccd)
{
    bx_chr_t *h = &dev->chr[dev->nchr++];
    memset(h, 0, sizeof(*h));
    h->value = value;
    h->decl = value - 1;
    h->props = props;
    h->svc = (uint8_t)svc_i;
    h->uuid = u;
    h->cccd = cccd;
}
static bx_uuid_t u16(uint16_t v) { return (bx_uuid_t){{(uint8_t)v, (uint8_t)(v >> 8)}, 2}; }
static void make_device(void)
{
    if (!dev) dev = malloc(sizeof(*dev));
    static const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    bx_begin(dev, 7, addr, 0, "MPT-II");
    dev->state = BX_READY;
    dev->phase = BX_P_DONE;
    dev->nsvc = 4;
    dev->svc[0].uuid = u16(0x1802);
    dev->svc[1].uuid = u16(0x1803);
    dev->svc[2].uuid = u16(0x18F0);
    dev->svc[3].uuid = u16(0x180F);
    add_chr(1, 10, 0x0A, u16(0x2A06), 0);        /* Link Loss alert level: read, write */
    add_chr(0, 12, 0x04, u16(0x2A06), 0);        /* Immediate Alert: write without response only */
    add_chr(2, 20, 0x0C, u16(0x2AF1), 0);
    add_chr(2, 22, 0x10, u16(0x2AF0), 23);
    add_chr(3, 30, 0x12, u16(0x2A19), 31);
    sent_reads = sent_writes = sent_subs = 0;
}
static lap_ble_event_t ev(int kind, uint16_t attr, int32_t error)
{
    lap_ble_event_t e;
    memset(&e, 0, sizeof(e));
    e.connection = 7;
    e.kind = (uint16_t)kind;
    e.attribute = attr;
    e.error = error;
    return e;
}
/* What the device does with a request: 0 answer, 1 refuse, 2 never answer. */
static int mood;
static void notify(uint16_t handle, const uint8_t *v, size_t n)
{
    lap_ble_event_t e = ev(LAP_GATT_NOTIFY, handle, 0);
    e.payload_size = (uint16_t)n;
    bx_event(dev, &e, v, n);
}
/* One frame: the runner's step, the board's answer the frame after. */
static bxf_req_t pending;
static bool has_pending;
static void frame(bxf_run_t *r, uint32_t ms)
{
    now += ms;
    if (has_pending) {
        has_pending = false;
        if (mood == 0 || mood == 1) {
            const int32_t err = mood ? LAP_IO_EIO : 0;
            if (pending.op.kind == BX_OP_READ) {
                lap_ble_event_t e = ev(LAP_GATT_VALUE, pending.op.a, err);
                const uint8_t v = 0x57;
                e.payload_size = 1;
                bx_event(dev, &e, &v, 1);
            } else {
                lap_ble_event_t e = ev(LAP_GATT_WRITTEN, pending.op.a, err);
                bx_event(dev, &e, NULL, 0);
            }
        }
    }
    bx_overdue(dev, now);
    bxf_req_t q;
    if (bxf_tick(r, dev, now, &q)) {
        LS_CHECK(!dev->busy);
        if (q.op.kind == BX_OP_READ) sent_reads++;
        else if (q.op.kind == BX_OP_WRITE) { sent_writes++; memcpy(last_write, q.data, q.n); last_wn = q.n; last_nr = q.no_rsp; }
        else sent_subs++;
        bxf_sent(r, dev, &q, 1, now);
        LS_CHECK(dev->busy);
        pending = q;
        has_pending = true;
    }
}
static int run_text(bxf_run_t *r, const char *t)
{
    bxf_lib_init(&lib);
    bxf_lib_add(&lib, t, strlen(t), BXF_ORIGIN_USER, "test.flow");
    LS_EQ_INT(lib.bad, 0);
    has_pending = false;
    mood = 0;
    return bxf_begin(r, &lib.e[0], dev, NULL, now);
}

LS_CASE(flow_runs_step_by_step) {
    static bxf_run_t r;
    make_device();
    const char *t = "flow Printer status\nabout x\nmatch service 18f0\n"
                    "notify 2af0 on\nwrite 2af1 10 04 01\nwait notify 2af0 2000\nlog all good\nwait 300\nread 2a19\nnotify 2af0 off\n";
    LS_EQ_INT(run_text(&r, t), 0);
    LS_EQ_INT(r.state, BXF_RUNNING);
    frame(&r, 16);                                       /* notify on goes out */
    LS_EQ_INT(sent_subs, 1);
    LS_EQ_INT(r.res[0].st, BXF_S_NOW);
    LS_EQ_UINT(pending.op.a, 23u);
    LS_CHECK(pending.op.on && pending.op.manual);
    frame(&r, 16);                                       /* written; noted ok */
    LS_EQ_INT(r.res[0].st, BXF_S_OK);
    LS_EQ_STR(r.res[0].text, "on");
    LS_CHECK(dev->chr[3].notifying);
    frame(&r, 16);                                       /* the write */
    LS_EQ_INT(sent_writes, 1);
    LS_EQ_INT(last_wn, 3);
    LS_CHECK(!last_nr);
    /* the reply lands before the wait starts and still counts */
    const uint8_t reply = 0x12;
    notify(22, &reply, 1);
    frame(&r, 16);
    LS_EQ_STR(r.res[1].text, "sent 3 bytes");
    frame(&r, 16);
    LS_EQ_INT(r.res[2].st, BXF_S_OK);
    LS_EQ_STR(r.res[2].text, "12");
    frame(&r, 16);
    LS_EQ_STR(r.res[3].text, "all good");
    frame(&r, 100);                                      /* the wait starts */
    LS_EQ_INT(r.res[4].st, BXF_S_NOW);
    frame(&r, 100);
    frame(&r, 100);
    LS_EQ_INT(r.res[4].st, BXF_S_NOW);
    frame(&r, 100);
    LS_EQ_INT(r.res[4].st, BXF_S_OK);
    frame(&r, 16);
    frame(&r, 16);
    LS_EQ_INT(sent_reads, 1);
    LS_EQ_STR(r.res[5].text, "87 %");
    frame(&r, 16);
    frame(&r, 16);
    LS_EQ_INT(r.state, BXF_DONE);
    LS_CHECK(!dev->chr[3].notifying);
    LS_EQ_INT(sent_subs, 2);
    /* nothing more goes out */
    frame(&r, 16);
    LS_EQ_INT(sent_subs + sent_writes + sent_reads, 4);
}

LS_CASE(flow_scoped_uuid_and_write_without_response) {
    static bxf_run_t r;
    make_device();
    LS_EQ_INT(run_text(&r, "flow Beep\nabout x\nmatch service 1802\nwrite 1802/2a06 02\nwrite 2a06 00\n"), 0);
    frame(&r, 16);
    LS_EQ_UINT(pending.op.a, 12u);                       /* Immediate Alert's, not Link Loss's */
    LS_CHECK(last_nr);
    frame(&r, 16);
    frame(&r, 16);
    LS_EQ_UINT(pending.op.a, 10u);                       /* unscoped: the first 2a06 */
    LS_CHECK(!last_nr);
    frame(&r, 16);
    LS_EQ_INT(r.state, BXF_DONE);
}

LS_CASE(flow_wait_for_notification_times_out) {
    static bxf_run_t r;
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nnotify 2af0 on\nwait notify 2af0 1000\nnotify 2af0 off\n"), 0);
    frame(&r, 16);
    frame(&r, 16);
    for (int i = 0; i < 9; i++) frame(&r, 100);
    LS_EQ_INT(r.state, BXF_RUNNING);
    frame(&r, 200);
    LS_EQ_INT(r.state, BXF_FAILED);
    LS_EQ_INT(r.res[1].st, BXF_S_FAIL);
    LS_EQ_STR(r.res[1].text, "nothing in 1000 ms");
    LS_EQ_STR(r.why, "Step 2: nothing in 1000 ms");
    frame(&r, 16);
    LS_EQ_INT(sent_subs, 1);                             /* stopped: the off step never ran */
    /* optional: noted, and the flow goes on to turn it off */
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nnotify 2af0 on\n? wait notify 2af0 1000\nnotify 2af0 off\n"), 0);
    for (int i = 0; i < 20; i++) frame(&r, 100);
    LS_EQ_INT(r.state, BXF_DONE);
    LS_EQ_INT(r.res[1].st, BXF_S_SKIP);
    LS_EQ_INT(sent_subs, 2);
}

LS_CASE(flow_lost_reply_and_error_stop) {
    static bxf_run_t r;
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nread 2a19\nread 2a19\n"), 0);
    mood = 2;                                            /* the device never answers */
    frame(&r, 16);
    for (int i = 0; i < 49; i++) frame(&r, 100);
    LS_EQ_INT(r.state, BXF_RUNNING);
    LS_CHECK(dev->busy);
    frame(&r, 200);                                      /* bx_overdue gives it up at 5 s */
    LS_EQ_INT(r.state, BXF_FAILED);
    LS_EQ_STR(r.res[0].text, "no reply in 5 s");
    LS_CHECK(!dev->busy);
    LS_EQ_INT(dev->state, BX_READY);                     /* the link stays */
    /* a refused write stops the flow at that step */
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nwrite 2af1 01\nread 2a19\n"), 0);
    mood = 1;
    frame(&r, 16);
    frame(&r, 16);
    LS_EQ_INT(r.state, BXF_FAILED);
    LS_EQ_STR(r.why, "Step 1: refused (may need pairing)");
    LS_EQ_INT(sent_reads, 0);
    /* a call the board refuses at once, a characteristic that is not there,
       one that cannot do it */
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\n? read 2a00\n? notify 2af1 on\n? read 2af0\nread 2a19\n"), 0);
    for (int i = 0; i < 3; i++) frame(&r, 16);
    LS_EQ_STR(r.res[0].text, "no 2a00 here");
    LS_EQ_STR(r.res[1].text, "cannot notify");
    LS_EQ_STR(r.res[2].text, "not readable");
    bxf_req_t q;
    LS_CHECK(bxf_tick(&r, dev, now, &q));
    bxf_sent(&r, dev, &q, LAP_IO_EBUSY, now);            /* busy: tried again next frame */
    LS_EQ_INT(r.state, BXF_RUNNING);
    LS_CHECK(bxf_tick(&r, dev, now, &q));
    bxf_sent(&r, dev, &q, LAP_IO_ENOTSUP, now);
    LS_EQ_INT(r.state, BXF_FAILED);
    LS_EQ_STR(r.why, "Step 4: not supported here");
    /* a lost link ends it; STOP ends it */
    make_device();
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nwait 1000\nread 2a19\n"), 0);
    frame(&r, 16);
    bxf_stop(&r, now);
    LS_EQ_INT(r.state, BXF_STOPPED);
    LS_EQ_STR(r.res[0].text, "stopped");
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nwait 1000\nread 2a19\n"), 0);
    lap_ble_event_t e = ev(LAP_BLE_DISCONNECTED, 0, 0);
    bx_event(dev, &e, NULL, 0);
    frame(&r, 16);
    LS_EQ_INT(r.state, BXF_FAILED);
    LS_EQ_STR(r.why, "Step 1: link lost");
    /* no flow starts before the walk is done */
    make_device();
    dev->phase = BX_P_READ_CHARS;
    LS_EQ_INT(run_text(&r, "flow Q\nabout x\nmatch name x\nread 2a19\n"), -1);
    LS_EQ_STR(r.why, "Wait for the table walk to finish");
}

LS_CASE(flow_wechat_airsync_split_packet) {
    bx_airsync_t a;
    memset(&a, 0, sizeof(a));
    char t[64];
    /* auth request, 26 bytes over two notifications */
    const uint8_t p1[20] = {0xfe, 0x01, 0x00, 0x1a, 0x27, 0x11, 0x00, 0x01, 0x0a, 0x02, 0x08, 0x00, 0x12, 0x10, 1, 2, 3, 4, 5, 6};
    const uint8_t p2[6] = {7, 8, 9, 10, 11, 12};
    LS_CHECK(bx_airsync(&a, 9, true, p1, sizeof(p1), t, sizeof(t)));
    LS_EQ_STR(t, "WeChat auth request, 20 of 26 bytes");
    LS_CHECK(bx_airsync(&a, 9, true, p2, sizeof(p2), t, sizeof(t)));
    LS_EQ_STR(t, "WeChat auth request, 26 bytes in 2 parts");
    LS_CHECK(!bx_airsync(&a, 9, true, p2, sizeof(p2), t, sizeof(t)));     /* done: a stray tail is not one */
    /* whole in one */
    const uint8_t init[12] = {0xfe, 0x01, 0x00, 0x0c, 0x4e, 0x23, 0x00, 0x02, 0, 0, 0, 0};
    LS_CHECK(bx_airsync(&a, 9, true, init, sizeof(init), t, sizeof(t)));
    LS_EQ_STR(t, "WeChat init response, 12 bytes");
    /* another characteristic's bytes do not join a packet in progress */
    LS_CHECK(bx_airsync(&a, 9, true, p1, sizeof(p1), t, sizeof(t)));
    LS_CHECK(!bx_airsync(&a, 4, false, p2, sizeof(p2), t, sizeof(t)));
    /* elsewhere only a known command counts */
    const uint8_t odd[8] = {0xfe, 0x01, 0x00, 0x08, 0x12, 0x34, 0, 0};
    LS_CHECK(!bx_airsync(&a, 4, false, odd, sizeof(odd), t, sizeof(t)));
    LS_CHECK(bx_airsync(&a, 9, true, odd, sizeof(odd), t, sizeof(t)));
    LS_EQ_STR(t, "WeChat command 4660, 8 bytes");
}
