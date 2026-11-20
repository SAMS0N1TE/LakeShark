/* ls_locker: the Wi-Fi file locker on hub.terminalbay.com. See core/ls_hub.h. */
#include "ls_hub.h"
#include "ls_hub_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#define INBOX "/sdcard/inbox"
#define MAX_FILE (25u * 1024u * 1024u)

static EXT_RAM_BSS_ATTR ls_locker_file_t s_files[LS_LOCKER_LIST_MAX];
static volatile int s_nfiles;
static char s_code[8];
static int64_t s_code_until;
static volatile int s_ready = -1;       /* 1 the locker exists, 0 the hub does not know this board yet */

bool ls_locker_pair(void) { return hub_submit(HOP_PAIR, "", false); }
bool ls_locker_list(void) { return hub_submit(HOP_LIST, "", false); }
bool ls_locker_upload(const char *p) { return p && hub_submit(HOP_UP, p, false); }
bool ls_locker_pull(void) { return hub_submit(HOP_PULL, "", false); }
bool ls_locker_unpair(void) { return hub_submit(HOP_UNPAIR, "", false); }
bool ls_locker_delete(const char *n) { return n && *n && hub_submit(HOP_DEL, n, false); }
bool ls_locker_get(const char *n) { return n && *n && hub_submit(HOP_GET, n, false); }
int  ls_locker_ready(void) { return s_ready; }
int  ls_locker_count(void) { return s_nfiles; }

bool ls_locker_file(int i, ls_locker_file_t *o)
{
    if (i < 0 || i >= s_nfiles) return false;
    *o = s_files[i];
    return true;
}

bool ls_locker_code(char out[8], uint32_t *left)
{
    const int64_t r = s_code_until - esp_timer_get_time();
    if (!s_code[0] || r <= 0) { out[0] = 0; if (left) *left = 0; return false; }
    memcpy(out, s_code, 8);
    if (left) *left = (uint32_t)(r / 1000000);
    return true;
}

static const char *why(int s, const char *body)
{
    static char t[72];
    if (s == -1) return "no Wi-Fi";
    if (s == -2) return "hub unreachable";
    if (s == -3) return "card or memory error";
    if (s == 429) return "too many tries, wait";
    if (!hub_json_str(body, "error", t, sizeof(t))) snprintf(t, sizeof(t), "failed (%d)", s);
    return t;
}

/* A name the card path /sdcard/inbox/<name> can take: the hub's own rule
   (no slash, no leading dot), checked here too in case the hub changes. */
static bool safe_name(const char *n)
{
    return n[0] && n[0] != '.' && !strchr(n, '/') && !strchr(n, '\\');
}

static int do_list(char *buf, uint32_t cap)
{
    hub_req_t q = { .method = "GET", .path = "/v1/locker/files?fmt=t", .out = buf, .ocap = cap };
    const int s = hub_call(&q, NULL);
    if (s == 401) { s_ready = 0; s_nfiles = 0; }
    if (s != 200) return s;
    s_ready = 1;
    int n = 0;
    for (char *l = buf; *l && n < LS_LOCKER_LIST_MAX;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char *f[4] = { l, NULL, NULL, NULL };
        int k = 1;
        for (char *p = l; *p && k < 4; p++)
            if (*p == '\t') { *p = 0; f[k++] = p + 1; }
        if (k == 4 && safe_name(f[0])) {
            ls_locker_file_t *o = &s_files[n++];
            strlcpy(o->name, f[0], sizeof(o->name));
            o->size = (uint32_t)strtoul(f[1], NULL, 10);
            o->age_s = (uint32_t)strtoul(f[2], NULL, 10);
            o->from_web = f[3][0] == 'w';
        }
        if (!e) break;
        l = e + 1;
    }
    s_nfiles = n;
    return 200;
}

/* The file name for the server: letters, digits . _ - and space; the rest become _. Returns the URL path. */
static void upload_name(const char *path, char *name, size_t nn, char *url, size_t nu)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t i = 0;
    for (; *b && i + 1 < nn && i < 63; b++, i++)
        name[i] = (isalnum((unsigned char)*b) || *b == '.' || *b == '_' || *b == '-' || *b == ' ') ? *b : '_';
    name[i] = 0;
    if (name[0] == '.' || name[0] == '-' || name[0] == ' ') name[0] = '_';
    size_t u = (size_t)snprintf(url, nu, "/v1/locker/files/");
    for (const char *p = name; *p && u + 4 < nu; p++) {
        if (*p == ' ') { memcpy(url + u, "%20", 3); u += 3; }
        else url[u++] = *p;
    }
    url[u] = 0;
}

/* "/v1/locker/files/<name>" with spaces as %20 (names are already in the server's alphabet). */
static void file_url(const char *name, char *url, size_t nu)
{
    size_t u = (size_t)snprintf(url, nu, "/v1/locker/files/");
    for (const char *p = name; *p && u + 4 < nu; p++) {
        if (*p == ' ') { memcpy(url + u, "%20", 3); u += 3; }
        else url[u++] = *p;
    }
    url[u] = 0;
}

void locker_run(hub_op_t op, const char *arg)
{
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) { hub_set(LS_HUB_FAIL, "locker", "out of memory"); return; }
    buf[0] = 0;
    int s;
    if (op == HOP_PAIR) {
        hub_req_t q = { .method = "POST", .path = "/v1/locker/pair", .out = buf, .ocap = 4096 };
        s = hub_call(&q, NULL);
        char code[8];
        if (s == 200 && hub_json_str(buf, "code", code, sizeof(code))) {
            strlcpy(s_code, code, sizeof(s_code));
            s_code_until = esp_timer_get_time() + 600LL * 1000000;
            s_ready = 1;
            hub_set(LS_HUB_OK, "pair", "code %s", code);
        } else hub_set(LS_HUB_FAIL, "pair", "%s", why(s, buf));
    } else if (op == HOP_LIST) {
        s = do_list(buf, 4096);
        if (s == 200) hub_set(LS_HUB_OK, "list", "%d files", s_nfiles);
        else hub_set(LS_HUB_FAIL, "list", "%s", why(s, buf));
    } else if (op == HOP_UP) {
        struct stat st;
        char name[64], url[220];
        /* Card files only: this task's stack is PSRAM, and a SPIFFS read
           turns the cache off under it (cache_utils assert). */
        if (strncmp(arg, "/sdcard/", 8) || strstr(arg, "/../")) hub_set(LS_HUB_FAIL, "upload", "card files only");
        else if (stat(arg, &st) || !S_ISREG(st.st_mode)) hub_set(LS_HUB_FAIL, "upload", "no such file");
        else if ((uint32_t)st.st_size > MAX_FILE) hub_set(LS_HUB_FAIL, "upload", "over 25 MB");
        else {
            upload_name(arg, name, sizeof(name), url, sizeof(url));
            hub_req_t q = { .method = "PUT", .path = url, .upfile = arg, .out = buf, .ocap = 4096 };
            s = hub_call(&q, NULL);
            if (s == 200) { do_list(buf, 4096); hub_set(LS_HUB_OK, "upload", "uploaded %s", name); }
            else hub_set(LS_HUB_FAIL, "upload", "%s", why(s, buf));
        }
    } else if (op == HOP_PULL) {
        s = do_list(buf, 4096);
        int got = 0, tried = 0;
        if (s == 200) {
            mkdir(INBOX, 0777);
            for (int i = 0; i < s_nfiles; i++) {
                if (!s_files[i].from_web) continue;
                char dst[96], url[220];
                struct stat st;
                snprintf(dst, sizeof(dst), INBOX "/%s", s_files[i].name);
                if (!stat(dst, &st) && (uint32_t)st.st_size == s_files[i].size) continue;
                file_url(s_files[i].name, url, sizeof(url));
                tried++;
                hub_req_t q = { .method = "GET", .path = url, .outfile = dst };
                if (hub_call(&q, NULL) == 200) got++;
            }
            if (got == tried) hub_set(LS_HUB_OK, "pull", got ? "%d new in /inbox" : "nothing new", got);
            else hub_set(LS_HUB_FAIL, "pull", "%d of %d came down", got, tried);
        } else hub_set(LS_HUB_FAIL, "pull", "%s", why(s, buf));
    } else if ((op == HOP_DEL || op == HOP_GET) && !safe_name(arg)) {
        hub_set(LS_HUB_FAIL, op == HOP_DEL ? "delete" : "get", "bad file name");
    } else if (op == HOP_DEL || op == HOP_GET) {
        char url[220], dst[96];
        file_url(arg, url, sizeof(url));
        snprintf(dst, sizeof(dst), INBOX "/%s", arg);
        if (op == HOP_GET) mkdir(INBOX, 0777);
        hub_req_t q = { .method = op == HOP_DEL ? "DELETE" : "GET", .path = url, .out = op == HOP_DEL ? buf : NULL,
                        .ocap = op == HOP_DEL ? 4096 : 0, .outfile = op == HOP_GET ? dst : NULL };
        s = hub_call(&q, NULL);
        if (s == 200 && op == HOP_DEL) {     /* drop it from the list now; the next list agrees */
            int k = 0;
            for (int i = 0; i < s_nfiles; i++) if (strcmp(s_files[i].name, arg)) s_files[k++] = s_files[i];
            s_nfiles = k;
        }
        if (s == 200) hub_set(LS_HUB_OK, op == HOP_DEL ? "delete" : "get", op == HOP_DEL ? "deleted %.40s" : "%.40s in /inbox", arg);
        else hub_set(LS_HUB_FAIL, op == HOP_DEL ? "delete" : "get", "%s", why(s, buf));
    } else if (op == HOP_UNPAIR) {
        hub_req_t q = { .method = "POST", .path = "/v1/locker/unpair", .out = buf, .ocap = 4096 };
        s = hub_call(&q, NULL);
        s_code[0] = 0;
        if (s == 200) hub_set(LS_HUB_OK, "unpair", "website and desktop sessions ended");
        else hub_set(LS_HUB_FAIL, "unpair", "%s", why(s, buf));
    }
    heap_caps_free(buf);
}

void locker_console(int argc, char **argv)
{
    const char *v = argc > 1 ? argv[1] : "";
    bool go = true;
    if (!strcmp(v, "pair")) ls_locker_pair();
    else if (!strcmp(v, "list")) ls_locker_list();
    else if (!strcmp(v, "up") && argc > 2) {
        /* Card paths the way `sd` takes them (/inbox/x.wav) as well as full ones (/sdcard/inbox/x.wav). */
        static char full[160];
        if (strncmp(argv[2], "/sdcard/", 8))
            snprintf(full, sizeof(full), "/sdcard%s%s", argv[2][0] == '/' ? "" : "/", argv[2]);
        else
            snprintf(full, sizeof(full), "%s", argv[2]);
        ls_locker_upload(full);
    }
    else if (!strcmp(v, "pull")) ls_locker_pull();
    else if (!strcmp(v, "unpair")) ls_locker_unpair();
    else if (!strcmp(v, "code")) {
        char c[8];
        uint32_t left;
        printf(ls_locker_code(c, &left) ? "locker: code %s, %us left\n" : "locker: no live code (locker pair)\n", c, (unsigned)left);
        go = false;
    } else {
        printf("locker pair | code | list | up <sd path> | pull | unpair\n");
        go = false;
    }
    if (!go) return;
    hub_wait(120000);
    ls_hub_status_t st;
    ls_hub_status(&st);
    printf("locker %s: %s\n", st.what, st.text);
    if (!strcmp(v, "list"))
        for (int i = 0; i < s_nfiles; i++)
            printf("  %-40s %9u  %us  %s\n", s_files[i].name, (unsigned)s_files[i].size, (unsigned)s_files[i].age_s, s_files[i].from_web ? "web" : "board");
}
