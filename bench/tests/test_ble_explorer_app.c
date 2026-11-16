/* BLE EXPLORER the SD app: its GATT engine (app-sdk/apps/ble_explorer/
   bx_core.c) as the app builds it, driven by a made-up peripheral that
   answers the way the board's NimBLE client reports (characteristic events
   carry the declaration handle). The whole walk, characteristic ends,
   descriptors and CCCDs, refused reads, a timeout, notifications, the SIG
   decoders and the saved JSON. */
#include "ls_test.h"
#include "bx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the made-up peripheral ------------------------------------------------------------ */
typedef struct { uint16_t start, end; uint8_t uuid[16], ulen; } fsvc_t;
typedef struct { uint16_t decl, value; uint8_t props; uint16_t uuid; const uint8_t *nus; const char *val; uint8_t vlen; int32_t err; } fchr_t;
typedef struct { uint16_t handle, uuid; uint8_t val[2], vlen; } fdsc_t;

static const uint8_t NUS_SVC[16] = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e};
static const uint8_t NUS_RX[16] = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e};
static const uint8_t NUS_TX[16] = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e};

static fsvc_t svcs[] = {
    {1, 5, {0x00, 0x18}, 2}, {6, 11, {0x0D, 0x18}, 2}, {12, 15, {0x0F, 0x18}, 2}, {16, 20, {0x0A, 0x18}, 2}, {21, 26, {0}, 16},
};
static fchr_t chrs[] = {
    {2, 3, 0x02, 0x2A00, NULL, "Forerunner", 10, 0},
    {4, 5, 0x02, 0x2A01, NULL, "\xc1\x00", 2, 0},
    {7, 8, 0x10, 0x2A37, NULL, NULL, 0, 0},
    {10, 11, 0x02, 0x2A38, NULL, "\x02", 1, 0},
    {13, 14, 0x12, 0x2A19, NULL, "\x57", 1, 0},
    {17, 18, 0x02, 0x2A29, NULL, "Garmin", 6, 0},
    {19, 20, 0x02, 0x2A50, NULL, "\x01\x87\x00\x34\x12\x01\x00", 7, LAP_IO_EIO},   /* needs pairing */
    {22, 23, 0x0C, 0, NUS_RX, NULL, 0, 0},
    {24, 25, 0x10, 0, NUS_TX, NULL, 0, 0},
};
static fdsc_t dscs[] = {
    {9, 0x2902, {0, 0}, 2}, {15, 0x2902, {0, 0}, 2}, {26, 0x2902, {0, 0}, 2},
};
static int procedures, timeout_at = -1, chr_ops;

static lap_ble_event_t ev(int kind, uint16_t attr)
{
    lap_ble_event_t e;
    memset(&e, 0, sizeof(e));
    e.connection = 7;
    e.kind = (uint16_t)kind;
    e.attribute = attr;
    return e;
}
static void uuid16(lap_uuid_t *u, uint16_t v) { u->length = 2; u->bytes[0] = (uint8_t)v; u->bytes[1] = (uint8_t)(v >> 8); }

/* Answer one procedure the way main/ble_lap_client.c reports it. */
static void answer(bx_conn_t *c, const bx_op_t *op)
{
    procedures++;
    lap_ble_event_t e;
    if (timeout_at >= 0 && procedures == timeout_at) {
        e = ev(LAP_GATT_COMPLETE, 0);
        e.error = LAP_IO_ETIMEOUT;
        bx_event(c, &e, NULL, 0);
        return;
    }
    if (op->kind == BX_OP_DISCOVER && !op->a) {
        for (size_t i = 0; i < sizeof(svcs) / sizeof(svcs[0]); i++) {
            e = ev(LAP_GATT_SERVICE, svcs[i].start);
            e.end_handle = svcs[i].end;
            e.uuid.length = svcs[i].ulen;
            memcpy(e.uuid.bytes, svcs[i].ulen == 16 ? NUS_SVC : svcs[i].uuid, svcs[i].ulen);
            bx_event(c, &e, NULL, 0);
        }
    } else if (op->kind == BX_OP_DISCOVER) {
        chr_ops++;
        for (size_t i = 0; i < sizeof(chrs) / sizeof(chrs[0]); i++) {
            if (chrs[i].value < op->a || chrs[i].value > op->b) continue;
            e = ev(LAP_GATT_CHARACTERISTIC, chrs[i].value);
            e.end_handle = chrs[i].decl;
            e.properties = chrs[i].props;
            if (chrs[i].nus) { e.uuid.length = 16; memcpy(e.uuid.bytes, chrs[i].nus, 16); }
            else uuid16(&e.uuid, chrs[i].uuid);
            bx_event(c, &e, NULL, 0);
        }
    } else if (op->kind == BX_OP_DESCRIPTORS) {
        for (size_t i = 0; i < sizeof(dscs) / sizeof(dscs[0]); i++) {
            if (dscs[i].handle <= op->a || dscs[i].handle > op->b) continue;
            e = ev(LAP_GATT_DESCRIPTOR, dscs[i].handle);
            uuid16(&e.uuid, dscs[i].uuid);
            bx_event(c, &e, NULL, 0);
        }
    } else if (op->kind == BX_OP_READ) {
        for (size_t i = 0; i < sizeof(chrs) / sizeof(chrs[0]); i++)
            if (chrs[i].value == op->a) {
                e = ev(LAP_GATT_VALUE, op->a);
                e.error = chrs[i].err;
                e.payload_size = chrs[i].err ? 0 : chrs[i].vlen;
                bx_event(c, &e, (const uint8_t *)chrs[i].val, e.payload_size);
                return;
            }
        for (size_t i = 0; i < sizeof(dscs) / sizeof(dscs[0]); i++)
            if (dscs[i].handle == op->a) {
                e = ev(LAP_GATT_VALUE, op->a);
                e.payload_size = dscs[i].vlen;
                bx_event(c, &e, dscs[i].val, dscs[i].vlen);
                return;
            }
        e = ev(LAP_GATT_VALUE, op->a);
        e.error = LAP_IO_EIO;
        bx_event(c, &e, NULL, 0);
        return;
    } else if (op->kind == BX_OP_SUBSCRIBE || op->kind == BX_OP_WRITE) {
        e = ev(LAP_GATT_WRITTEN, op->a);
        bx_event(c, &e, NULL, 0);
        return;
    }
    e = ev(LAP_GATT_COMPLETE, 0);
    bx_event(c, &e, NULL, 0);
}

static bx_conn_t *conn;
static void connect_and_walk(void)
{
    static const uint8_t addr[6] = {0x0c, 0xfe, 0x11, 0x72, 0x39, 0xd0};
    if (!conn) conn = malloc(sizeof(*conn));
    bx_begin(conn, 7, addr, 0, "Forerunner 265");
    procedures = chr_ops = 0;
    lap_ble_event_t e = ev(LAP_BLE_CONNECTED, 0);
    bx_event(conn, &e, NULL, 0);
    for (int guard = 0; guard < 200; guard++) {
        const bx_op_t op = bx_next(conn);
        if (op.kind == BX_OP_NONE) break;
        bx_started(conn, &op);
        LS_CHECK(bx_next(conn).kind == BX_OP_NONE);        /* one procedure at a time */
        answer(conn, &op);
    }
}
static const bx_chr_t *chr_uuid(uint16_t u)
{
    for (int i = 0; i < conn->nchr; i++) if (bx_uuid16(&conn->chr[i].uuid) == u) return &conn->chr[i];
    return NULL;
}

LS_CASE(ble_explorer_walks_the_whole_table) {
    timeout_at = -1;
    connect_and_walk();
    LS_EQ_INT(conn->state, BX_READY);
    LS_EQ_INT(conn->phase, BX_P_DONE);
    LS_EQ_INT(bx_progress(conn), 100);
    LS_EQ_INT(conn->nsvc, 5);
    LS_EQ_INT(conn->nchr, 9);
    LS_EQ_INT(conn->ndsc, 3);
    LS_EQ_INT(chr_ops, 5);
    /* 1 services + 5 characteristic + 3 descriptor walks + 6 reads + 3 descriptor reads */
    LS_EQ_INT(procedures, 1 + 5 + 3 + 6 + 3);
    /* characteristic ends: up to the next declaration, else the service end */
    LS_EQ_INT(chr_uuid(0x2A00)->end, 3);
    LS_EQ_INT(chr_uuid(0x2A01)->end, 5);
    LS_EQ_INT(chr_uuid(0x2A37)->end, 9);
    LS_EQ_INT(chr_uuid(0x2A19)->end, 15);
    LS_EQ_INT(chr_uuid(0x2A37)->cccd, 9);
    LS_EQ_INT(chr_uuid(0x2A19)->cccd, 15);
    LS_EQ_INT(chr_uuid(0x2A00)->cccd, 0);
    LS_EQ_INT(chr_uuid(0x2A19)->decl, 13);
    LS_EQ_INT(chr_uuid(0x2A19)->svc, 2);
    /* reads, and the one that needed pairing */
    char t[96];
    LS_CHECK(bx_decode(0x2A00, chr_uuid(0x2A00)->val, chr_uuid(0x2A00)->vlen, t, sizeof(t)));
    LS_EQ_STR(t, "Forerunner");
    LS_CHECK(bx_decode(0x2A19, chr_uuid(0x2A19)->val, chr_uuid(0x2A19)->vlen, t, sizeof(t)));
    LS_EQ_STR(t, "87 %");
    LS_CHECK(bx_decode(0x2A01, chr_uuid(0x2A01)->val, chr_uuid(0x2A01)->vlen, t, sizeof(t)));
    LS_EQ_STR(t, "Watch (0x00c1)");
    LS_CHECK(bx_decode(0x2A38, chr_uuid(0x2A38)->val, 1, t, sizeof(t)));
    LS_EQ_STR(t, "wrist");
    LS_EQ_INT(chr_uuid(0x2A50)->error, LAP_IO_EIO);
    LS_CHECK(!chr_uuid(0x2A50)->has_value);
    LS_EQ_UINT(conn->errors, 1u);
    LS_CHECK(!chr_uuid(0x2A37)->has_value);                /* notify only: never read */
    /* the Nordic UART service by its 128-bit UUID */
    LS_EQ_STR(bx_service_name(&conn->svc[4].uuid), "Nordic UART");
    char u[40];
    bx_uuid_text(&conn->svc[4].uuid, u, sizeof(u));
    LS_EQ_STR(u, "6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    LS_EQ_STR(bx_char_name(&conn->chr[8].uuid), "UART TX (notify)");
    LS_EQ_INT(conn->chr[8].cccd, 26);
}

LS_CASE(ble_explorer_subscribes_and_logs_notifications) {
    timeout_at = -1;
    connect_and_walk();
    bx_chr_t *hr = NULL;
    for (int i = 0; i < conn->nchr; i++) if (bx_uuid16(&conn->chr[i].uuid) == 0x2A37) hr = &conn->chr[i];
    LS_CHECK(hr != NULL);
    bx_op_t op = {.kind = BX_OP_SUBSCRIBE, .a = hr->cccd, .manual = true, .on = true};
    bx_started(conn, &op);
    answer(conn, &op);
    LS_CHECK(hr->notifying);
    LS_CHECK(!conn->busy);
    LS_EQ_INT(conn->phase, BX_P_DONE);                   /* a manual request never moves the walk */
    lap_ble_event_t e = ev(LAP_GATT_NOTIFY, hr->value);
    const uint8_t m[] = {0x16, 72, 0x10, 0x03};          /* contact, 72 bpm, RR 784/1024 s */
    e.payload_size = sizeof(m);
    LS_CHECK(bx_event(conn, &e, m, sizeof(m)));
    LS_EQ_UINT(hr->notes, 1u);
    char t[96];
    LS_CHECK(bx_decode(0x2A37, hr->val, hr->vlen, t, sizeof(t)));
    LS_EQ_STR(t, "72 bpm, contact, RR 765 ms");
    /* the CCCD's shown value follows */
    char d[48];
    for (int i = 0; i < conn->ndsc; i++)
        if (conn->dsc[i].handle == hr->cccd) {
            LS_CHECK(bx_decode_desc(0x2902, conn->dsc[i].val, conn->dsc[i].vlen, d, sizeof(d)));
            LS_EQ_STR(d, "notifications on");
        }
    op.on = false;
    bx_started(conn, &op);
    answer(conn, &op);
    LS_CHECK(!hr->notifying);
    /* an error notification is not counted */
    e.error = LAP_IO_ELIMIT;
    LS_CHECK(!bx_event(conn, &e, NULL, 0));
    LS_EQ_UINT(hr->notes, 1u);
}

LS_CASE(ble_explorer_timeout_and_refusals) {
    timeout_at = 4;                                      /* the third characteristic walk times out */
    connect_and_walk();
    LS_EQ_INT(conn->state, BX_GONE);
    LS_EQ_INT(conn->error, LAP_IO_ETIMEOUT);
    LS_EQ_INT(bx_next(conn).kind, BX_OP_NONE);
    timeout_at = -1;
    /* a refused start: busy is retried, anything else is skipped */
    connect_and_walk();
    bx_rewalk(conn);
    bx_op_t op = bx_next(conn);
    LS_EQ_INT(op.kind, BX_OP_DISCOVER);
    bx_refused(conn, &op, LAP_IO_EBUSY);
    LS_EQ_INT(conn->phase, BX_P_SERVICES);
    bx_refused(conn, &op, LAP_IO_EIO);
    LS_EQ_INT(conn->phase, BX_P_CHARS);
    bx_refused(conn, &op, LAP_IO_ECLOSED);
    LS_EQ_INT(conn->state, BX_GONE);
    /* a failed connect */
    static const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    bx_begin(conn, 9, addr, 1, "");
    lap_ble_event_t e = ev(LAP_BLE_CONNECTED, 0);
    e.connection = 9;
    e.error = LAP_IO_EBUSY;
    bx_event(conn, &e, NULL, 0);
    LS_EQ_INT(conn->state, BX_FAILED);
    LS_EQ_STR(bx_error_text(conn->error), "busy");
}

LS_CASE(ble_explorer_full_tables_are_marked) {
    static const uint8_t addr[6] = {1, 2, 3, 4, 5, 6};
    if (!conn) conn = malloc(sizeof(*conn));
    bx_begin(conn, 7, addr, 1, "");
    lap_ble_event_t e = ev(LAP_BLE_CONNECTED, 0);
    bx_event(conn, &e, NULL, 0);
    for (int i = 0; i < BX_MAX_SVC + 3; i++) {
        e = ev(LAP_GATT_SERVICE, (uint16_t)(1 + i * 2));
        e.end_handle = (uint16_t)(2 + i * 2);
        uuid16(&e.uuid, 0x1800);
        bx_event(conn, &e, NULL, 0);
    }
    LS_EQ_INT(conn->nsvc, BX_MAX_SVC);
    LS_CHECK(conn->truncated);
}

LS_CASE(ble_explorer_decoders) {
    char t[96];
    const uint8_t hr16[] = {0x01, 0x2C, 0x01};                   /* 16-bit rate: 300 bpm */
    LS_CHECK(bx_decode(0x2A37, hr16, 3, t, sizeof(t)));
    LS_EQ_STR(t, "300 bpm");
    const uint8_t hre[] = {0x0C, 60, 0x10, 0x00};                /* contact supported, not in contact, energy */
    LS_CHECK(bx_decode(0x2A37, hre, 4, t, sizeof(t)));
    LS_EQ_STR(t, "60 bpm, no contact, 16 kJ");
    const uint8_t tm[] = {0x00, 0x6A, 0x0E, 0x00, 0xFE};          /* 3690 x 10^-2 C */
    LS_CHECK(bx_decode(0x2A1C, tm, 5, t, sizeof(t)));
    LS_EQ_STR(t, "36.90 C");
    const uint8_t temp[] = {0x2A, 0x09};                         /* 2346 x 0.01 */
    LS_CHECK(bx_decode(0x2A6E, temp, 2, t, sizeof(t)));
    LS_EQ_STR(t, "23.46 C");
    const uint8_t neg[] = {0x06, 0xFF};                           /* -250 x 0.01 */
    LS_CHECK(bx_decode(0x2A6E, neg, 2, t, sizeof(t)));
    LS_EQ_STR(t, "-2.50 C");
    const uint8_t hum[] = {0x88, 0x13};
    LS_CHECK(bx_decode(0x2A6F, hum, 2, t, sizeof(t)));
    LS_EQ_STR(t, "50.00 %");
    const uint8_t pr[] = {0x88, 0x75, 0x0F, 0x00};                /* 1013128 x 0.1 Pa */
    LS_CHECK(bx_decode(0x2A6D, pr, 4, t, sizeof(t)));
    LS_EQ_STR(t, "1013.1 hPa");
    const uint8_t pnp[] = {0x01, 0x87, 0x00, 0x34, 0x12, 0x01, 0x00};
    LS_CHECK(bx_decode(0x2A50, pnp, 7, t, sizeof(t)));
    LS_EQ_STR(t, "SIG vendor 0x0087 product 0x1234 version 0x0001");
    const uint8_t ct[] = {0xEA, 0x07, 11, 13, 9, 30, 5, 4};
    LS_CHECK(bx_decode(0x2A2B, ct, 8, t, sizeof(t)));
    LS_EQ_STR(t, "2026-11-13 09:30:05");
    const uint8_t tx = (uint8_t)-8;
    LS_CHECK(bx_decode(0x2A07, &tx, 1, t, sizeof(t)));
    LS_EQ_STR(t, "-8 dBm");
    const uint8_t pcp[] = {0x18, 0x00, 0x28, 0x00, 0x00, 0x00, 0x90, 0x01};
    LS_CHECK(bx_decode(0x2A04, pcp, 8, t, sizeof(t)));
    LS_EQ_STR(t, "interval 30-50 ms, latency 0, timeout 4000 ms");
    const uint8_t csc[] = {0x03, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0x07, 0x00, 0x00, 0x08};
    LS_CHECK(bx_decode(0x2A5B, csc, sizeof(csc), t, sizeof(t)));
    LS_EQ_STR(t, "wheel 16 revs, crank 7 revs");
    /* unknown characteristic: printable text quoted, binary not decoded */
    LS_CHECK(bx_decode(0xFFF1, (const uint8_t *)"v1.2\0", 5, t, sizeof(t)));
    LS_EQ_STR(t, "\"v1.2\"");
    const uint8_t bin[] = {0x00, 0xFF, 0x10};
    LS_CHECK(!bx_decode(0xFFF1, bin, 3, t, sizeof(t)));
    LS_CHECK(!bx_decode(0x2A19, bin, 0, t, sizeof(t)));       /* empty: nothing to say */
    bx_hex(bin, 3, t, sizeof(t));
    LS_EQ_STR(t, "00 ff 10");
    /* descriptors */
    const uint8_t ind[] = {2, 0}, fmt[] = {0x0E, 0xFE, 0x2F, 0x27, 1, 0, 0}, rr[] = {3, 1};
    LS_CHECK(bx_decode_desc(0x2902, ind, 2, t, sizeof(t)));
    LS_EQ_STR(t, "indications on");
    LS_CHECK(bx_decode_desc(0x2904, fmt, 7, t, sizeof(t)));
    LS_EQ_STR(t, "format 0x0e exp -2 unit 0x272f");
    LS_CHECK(bx_decode_desc(0x2908, rr, 2, t, sizeof(t)));
    LS_EQ_STR(t, "report 3 input");
    LS_CHECK(bx_decode_desc(0x2901, (const uint8_t *)"Speed", 5, t, sizeof(t)));
    LS_EQ_STR(t, "Speed");
    char p[8];
    bx_props_text(0x3A, p, sizeof(p));
    LS_EQ_STR(p, "RWNI");
    /* a SIG UUID written out long is still the short one */
    bx_uuid_t lng = {{0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x0F, 0x18, 0x00, 0x00}, 16};
    LS_EQ_INT(bx_uuid16(&lng), 0x180F);
    LS_EQ_STR(bx_service_name(&lng), "Battery");
}

LS_CASE(ble_explorer_json) {
    timeout_at = -1;
    connect_and_walk();
    bx_note_t n = {.at_ms = 1500, .handle = 8, .uuid16 = 0x2A37, .val = {0x16, 72}, .vlen = 2};
    const size_t need = bx_json(conn, &n, 1, 1792756800, NULL, 0);
    LS_CHECK(need > 500);
    char *buf = malloc(need + 1);
    LS_EQ_UINT(bx_json(conn, &n, 1, 1792756800, buf, need + 1), need);
    LS_EQ_UINT(strlen(buf), need);
    LS_CHECK(strstr(buf, "\"address\": \"d0:39:72:11:fe:0c\"") != NULL);
    LS_CHECK(strstr(buf, "\"name\": \"Forerunner 265\"") != NULL);
    LS_CHECK(strstr(buf, "\"complete\": true") != NULL);
    LS_CHECK(strstr(buf, "\"uuid\": \"2a19\", \"name\": \"Battery Level\"") != NULL);
    LS_CHECK(strstr(buf, "\"value_hex\": \"57\", \"decoded\": \"87 %\"") != NULL);
    LS_CHECK(strstr(buf, "\"error\": \"refused (may need pairing)\"") != NULL);
    LS_CHECK(strstr(buf, "\"uuid\": \"6e400001-b5a3-f393-e0a9-e50e24dcca9e\", \"name\": \"Nordic UART\"") != NULL);
    LS_CHECK(strstr(buf, "{\"ms\": 1500, \"handle\": 8, \"value_hex\": \"16 48\"}") != NULL);
    /* balanced brackets and braces outside strings */
    int depth = 0, sq = 0;
    bool in = false;
    for (const char *p = buf; *p; p++) {
        if (*p == '\\' && in) { p++; continue; }
        if (*p == '"') in = !in;
        if (in) continue;
        depth += (*p == '{') - (*p == '}');
        sq += (*p == '[') - (*p == ']');
        LS_CHECK(depth >= 0 && sq >= 0);
    }
    LS_EQ_INT(depth, 0);
    LS_EQ_INT(sq, 0);
    LS_CHECK(!in);
    /* a short buffer is never overrun and reports the size it needs */
    char small[64];
    memset(small, 'x', sizeof(small));
    LS_EQ_UINT(bx_json(conn, &n, 1, 1792756800, small, 32), need);
    LS_CHECK(small[31] == 0 && small[32] == 'x');
    /* a quote in a name is escaped */
    snprintf(conn->name, sizeof(conn->name), "a\"b\\c");
    bx_json(conn, NULL, 0, 0, buf, need + 1);
    LS_CHECK(strstr(buf, "\"name\": \"a\\\"b\\\\c\"") != NULL);
    free(buf);
}
