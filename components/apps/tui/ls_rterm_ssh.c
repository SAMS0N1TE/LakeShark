/* See ls_rterm_ssh.h.

   The library is libssh 0.12 (david-cermak/libssh, the ESP-IDF port), on
   the IDF's own mbedTLS. One session at a time; a second client is turned
   away at the door while it lasts.

   The host key is Ed25519, made on the first start and kept in NVS, so the
   client's known_hosts stays right across restarts and updates. A client
   gets in with a key on the list (NVS, and the SD card's
   lakeshark/authorized_keys when there is one), or with the terminal
   password, the same one telnet asks for. 'Learn' keeps the key a client
   offers on its next password login, so a phone that has a key never needs
   the password again. */

#include "ls_rterm_ssh.h"
#include "sdkconfig.h"

#if !CONFIG_LS_SSH
/* Built without SSH (CONFIG_LS_SSH off: libssh is ~230 KB the T-Display's app
   slot cannot spare). Telnet and the web screen are unchanged. */
#include <stdio.h>
#include <string.h>
void rterm_ssh_load(void) {}
void rterm_ssh_status(ls_rterm_status_t *o) { (void)o; }
void rterm_ssh_kick(void) {}
bool ls_rterm_ssh_set(bool on) { return !on; }
bool ls_rterm_ssh_port_set(int port) { (void)port; return false; }
void ls_rterm_ssh_learn(bool on) { (void)on; }
bool ls_rterm_ssh_keys_clear(void) { return false; }
int rterm_ssh_console(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("term: this build has no SSH; telnet ('term net on') and the web screen work\n");
    return 1;
}
#else

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "ls_nvs_safe.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

#include "ls_vt.h"

#define SSH_PORT_DEFAULT 22
#define SSH_STACK        24576
#define SSH_AUTH_US      60000000        /* to get past the login and ask for a shell */
#define SSH_LEARN_US     120000000       /* how long 'learn' waits for a login */
#define KEYS_MAX         8
#define KEYS_BYTES       3072            /* NVS holds a string up to 4000 */
#define KEY_LINE         1100            /* an RSA 4096 key's line is about 750 */
#define HOST_BYTES       640
#define NVS_NS           "lsterm"
#define SD_KEYS          "/sdcard/lakeshark/authorized_keys"

/* Settings. The key list and the fingerprint go under s_kmu. */
static volatile bool s_enabled, s_kick, s_rebind;
static volatile int s_port = SSH_PORT_DEFAULT;
static EXT_RAM_BSS_ATTR char s_host[HOST_BYTES];   /* the host key, OpenSSH private key text */
static EXT_RAM_BSS_ATTR char s_keys[KEYS_BYTES];   /* authorized_keys lines */
static char s_fp[56];                              /* SHA256:... of the host key */
static int s_sd_keys;                              /* lines on the SD card, as last read */
static int64_t s_learn_until;
static SemaphoreHandle_t s_kmu;
static TaskHandle_t s_task;

/* The connection. Only the SSH task touches these but for the status. */
static volatile bool s_listening, s_client, s_authed;
static volatile uint32_t s_client_ip;
static ssh_bind s_bind;
static ssh_channel s_chan;
static bool s_viewing, s_end, s_want_ask;
static int s_tries, s_pty_cols, s_pty_rows;
static EXT_RAM_BSS_ATTR char s_offered[KEY_LINE];  /* the first key the client offered, for 'learn' */

/* --- keys -------------------------------------------------------------- */

static int count_lines(const char *p)
{
    int n = 0;
    for (; *p; p++)
        if (*p == '\n') n++;
    return n;
}

/* "type base64 [comment]", up to the end of the line: the base64, cut out
   into b64. A line that opens with options (from=..., no-pty) is passed over. */
static bool line_key(const char *line, char *b64, size_t cap)
{
    const char *end = line + strcspn(line, "\r\n");
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (line == end || *line == '#') return false;
    const char *sp = line;
    while (sp < end && *sp != ' ' && *sp != '\t') sp++;
    char type[32];
    const size_t tn = (size_t)(sp - line);
    if (sp == end || tn >= sizeof(type)) return false;
    memcpy(type, line, tn);
    type[tn] = 0;
    if (ssh_key_type_from_name(type) == SSH_KEYTYPE_UNKNOWN) return false;
    while (sp < end && (*sp == ' ' || *sp == '\t')) sp++;
    const char *e = sp;
    while (e < end && *e != ' ' && *e != '\t') e++;
    const size_t bn = (size_t)(e - sp);
    if (!bn || bn >= cap) return false;
    memcpy(b64, sp, bn);
    b64[bn] = 0;
    return true;
}

/* A public key the same as one on the list: its base64 is the same text. */
static bool listed_in(const char *text, const char *want, char *scratch)
{
    for (; *text; text += strcspn(text, "\n"), text += *text ? 1 : 0)
        if (line_key(text, scratch, KEY_LINE) && !strcmp(scratch, want)) return true;
    return false;
}

/* The SD card's list, read each time: it may have been swapped. Counts its
   keys as it goes. */
static bool listed_on_sd(const char *want, char *scratch)
{
    FILE *f = fopen(SD_KEYS, "r");
    if (!f) { s_sd_keys = 0; return false; }
    char *line = malloc(KEY_LINE);
    bool hit = false;
    int n = 0;
    while (line && fgets(line, KEY_LINE, f))
        if (line_key(line, scratch, KEY_LINE)) {
            n++;
            if (!strcmp(scratch, want)) hit = true;
        }
    free(line);
    fclose(f);
    s_sd_keys = n;
    return hit;
}

static bool key_allowed(ssh_key key)
{
    char *b64 = NULL;
    if (ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK || !b64) return false;
    char *scratch = malloc(KEY_LINE);
    bool ok = false;
    if (scratch) {
        xSemaphoreTake(s_kmu, portMAX_DELAY);
        ok = listed_in(s_keys, b64, scratch);
        xSemaphoreGive(s_kmu);
        if (!ok) ok = listed_on_sd(b64, scratch);
    }
    free(scratch);
    ssh_string_free_char(b64);
    return ok;
}

static esp_err_t save_job(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    xSemaphoreTake(s_kmu, portMAX_DELAY);
    esp_err_t e = nvs_set_u8(h, "ssh", s_enabled ? 1 : 0);
    if (!e) e = nvs_set_u16(h, "sshport", (uint16_t)s_port);
    if (!e) e = nvs_set_str(h, "sshkeys", s_keys);
    if (!e && s_host[0]) e = nvs_set_str(h, "sshhost", s_host);
    xSemaphoreGive(s_kmu);
    if (!e) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* NVS from a task whose stack may be in PSRAM (this one, the screen's). */
static bool save(void) { return ls_nvs_run(save_job, NULL, 0) == ESP_OK; }

/* One more key, "type base64 comment". False when the list is full or the
   line is not a key; true for one it already has. */
static bool key_add(const char *line)
{
    char *b64 = malloc(2 * KEY_LINE), *scratch = b64 + KEY_LINE;   /* the console's stack is small */
    if (!b64) return false;
    bool ok = false, have = false;
    if (line_key(line, b64, KEY_LINE)) {
        const size_t n = strcspn(line, "\r\n");
        xSemaphoreTake(s_kmu, portMAX_DELAY);
        have = listed_in(s_keys, b64, scratch);
        if (!have && count_lines(s_keys) < KEYS_MAX && strlen(s_keys) + n + 2 <= KEYS_BYTES) {
            strncat(s_keys, line, n);
            strcat(s_keys, "\n");
            ok = true;
        }
        xSemaphoreGive(s_kmu);
    }
    free(b64);
    return have || (ok && save());
}

static bool key_del(int index)
{
    xSemaphoreTake(s_kmu, portMAX_DELAY);
    char *p = s_keys;
    for (int i = 0; i < index && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    char *nl = p && *p ? strchr(p, '\n') : NULL;
    if (nl) memmove(p, nl + 1, strlen(nl + 1) + 1);
    xSemaphoreGive(s_kmu);
    return nl && save();
}

/* --- the session's callbacks --------------------------------------------- */

static void chan_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    while (n && s_chan && !s_end) {
        const int k = ssh_channel_write(s_chan, b, (uint32_t)n);
        if (k <= 0) { s_end = true; return; }
        b += k;
        n -= (size_t)k;
    }
}

static int on_auth_none(ssh_session ses, const char *user, void *ud)
{
    (void)ses; (void)user; (void)ud;
    return SSH_AUTH_DENIED;
}

static int on_auth_password(ssh_session ses, const char *user, const char *pass, void *ud)
{
    (void)user; (void)ud;
    if (!rterm_pass_ok(pass)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++s_tries >= 3) ssh_disconnect(ses);
        return SSH_AUTH_DENIED;
    }
    s_authed = true;
    if (s_offered[0] && esp_timer_get_time() < s_learn_until && key_add(s_offered)) s_learn_until = 0;
    return SSH_AUTH_SUCCESS;
}

static int on_auth_pubkey(ssh_session ses, const char *user, struct ssh_key_struct *key, char state, void *ud)
{
    (void)ses; (void)user; (void)ud;
    if (key_allowed(key)) {
        if (state == SSH_PUBLICKEY_STATE_NONE) return SSH_AUTH_SUCCESS;   /* yes, sign with it */
        if (state == SSH_PUBLICKEY_STATE_VALID) { s_authed = true; return SSH_AUTH_SUCCESS; }
        return SSH_AUTH_DENIED;
    }
    /* Learning: the first key it offers, kept if the password then comes.
       Anyone with the password could put a key on the list anyway. */
    char *b64 = NULL;
    if (!s_offered[0] && esp_timer_get_time() < s_learn_until &&
        ssh_pki_export_pubkey_base64(key, &b64) == SSH_OK && b64) {
        snprintf(s_offered, sizeof(s_offered), "%s %s learned", ssh_key_type_to_char(ssh_key_type(key)), b64);
    }
    ssh_string_free_char(b64);
    return SSH_AUTH_DENIED;
}

static int on_pty(ssh_session ses, ssh_channel ch, const char *term, int cols, int rows, int pxw, int pxh, void *ud)
{
    (void)ses; (void)ch; (void)term; (void)pxw; (void)pxh; (void)ud;
    s_pty_cols = cols;
    s_pty_rows = rows;
    return 0;
}

static int on_window(ssh_session ses, ssh_channel ch, int cols, int rows, int pxw, int pxh, void *ud)
{
    (void)ses; (void)ch; (void)pxw; (void)pxh; (void)ud;
    if (s_viewing) rterm_ssh_size(cols, rows);
    else { s_pty_cols = cols; s_pty_rows = rows; }
    return SSH_OK;
}

static int on_shell(ssh_session ses, ssh_channel ch, void *ud)
{
    (void)ses; (void)ch; (void)ud;
    if (!s_authed || s_viewing) return 1;
    /* The size question goes from the loop: no writes from inside a callback. */
    s_want_ask = rterm_ssh_begin(s_pty_cols, s_pty_rows);
    s_viewing = true;
    return 0;
}

static int on_exec(ssh_session ses, ssh_channel ch, const char *cmd, void *ud)
{
    (void)ses; (void)ch; (void)cmd; (void)ud;
    return 1;                                        /* the screen, or nothing */
}

static int on_data(ssh_session ses, ssh_channel ch, void *data, uint32_t len, int is_stderr, void *ud)
{
    (void)ses; (void)ch; (void)is_stderr; (void)ud;
    if (s_viewing) rterm_ssh_input(data, len);
    return (int)len;
}

static void on_eof(ssh_session ses, ssh_channel ch, void *ud)
{
    (void)ses; (void)ch; (void)ud;
    s_end = true;
}

static struct ssh_channel_callbacks_struct s_chan_cb = {
    .channel_data_function = on_data,
    .channel_eof_function = on_eof,
    .channel_close_function = on_eof,
    .channel_pty_request_function = on_pty,
    .channel_shell_request_function = on_shell,
    .channel_pty_window_change_function = on_window,
    .channel_exec_request_function = on_exec,
};

static ssh_channel on_channel_open(ssh_session ses, void *ud)
{
    (void)ud;
    if (s_chan || !s_authed) return NULL;
    s_chan = ssh_channel_new(ses);
    if (!s_chan) return NULL;
    ssh_callbacks_init(&s_chan_cb);
    ssh_set_channel_callbacks(s_chan, &s_chan_cb);
    return s_chan;
}

static struct ssh_server_callbacks_struct s_server_cb = {
    .auth_none_function = on_auth_none,
    .auth_password_function = on_auth_password,
    .auth_pubkey_function = on_auth_pubkey,
    .channel_open_request_session_function = on_channel_open,
};

/* --- the task ------------------------------------------------------------ */

/* A second client while one is on: closed at once. */
static void refuse_others(void)
{
    const int fd = s_bind ? ssh_bind_get_fd(s_bind) : -1;
    if (fd < 0) return;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = {0};
    if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) return;
    const int c = accept(fd, NULL, NULL);
    if (c >= 0) close(c);
}

static void serve(ssh_session ses)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    s_client_ip = getpeername(ssh_get_fd(ses), (struct sockaddr *)&peer, &plen) ? 0 : peer.sin_addr.s_addr;
    s_client = true;
    s_authed = s_viewing = s_end = s_want_ask = false;
    s_chan = NULL;
    s_tries = s_pty_cols = s_pty_rows = 0;
    s_offered[0] = 0;
    long timeout = 10;
    ssh_options_set(ses, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_callbacks_init(&s_server_cb);
    ssh_set_server_callbacks(ses, &s_server_cb);
    ssh_event ev = NULL;
    if (ssh_handle_key_exchange(ses) != SSH_OK) goto out;
    ssh_set_auth_methods(ses, SSH_AUTH_METHOD_PUBLICKEY | SSH_AUTH_METHOD_PASSWORD);
    ev = ssh_event_new();
    if (!ev || ssh_event_add_session(ev, ses) != SSH_OK) goto out;
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (ssh_event_dopoll(ev, 20) == SSH_ERROR) break;
        if (s_end || s_kick || s_rebind || !s_enabled || !ssh_is_connected(ses)) break;
        refuse_others();
        if (!s_viewing) {
            if (s_tries >= 3 || esp_timer_get_time() - t0 > SSH_AUTH_US) break;
            continue;
        }
        if (s_want_ask) {
            s_want_ask = false;
            ls_vt_ask_size(chan_out, NULL);
        }
        if (rterm_ssh_quit()) break;
        const char *b;
        size_t n;
        if (rterm_ssh_take(&b, &n)) {
            chan_out(NULL, b, n);
            rterm_ssh_taken();
        }
    }
out:
    if (s_viewing) {
        rterm_ssh_end();
        if (!s_end) ls_vt_leave(chan_out, NULL);
    }
    if (s_chan && ssh_channel_is_open(s_chan)) {
        ssh_channel_request_send_exit_status(s_chan, 0);   /* a clean end: ssh exits 0 */
        ssh_channel_send_eof(s_chan);
        ssh_channel_close(s_chan);
    }
    if (ev) ssh_event_free(ev);
    ssh_disconnect(ses);
    s_chan = NULL;
    s_viewing = s_authed = s_client = false;
    s_kick = false;
}

static void unbind(void)
{
    if (s_bind) ssh_bind_free(s_bind);
    s_bind = NULL;
    s_listening = false;
}

static bool host_key_ready(void)
{
    ssh_key key = NULL;
    if (s_host[0] && ssh_pki_import_privkey_base64(s_host, NULL, NULL, NULL, &key) != SSH_OK) key = NULL;
    if (!key) {
        /* The first start: a key of this board's own. */
        char *text = NULL;
        if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, NULL, &key) != SSH_OK ||
            ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &text) != SSH_OK || !text ||
            strlen(text) >= sizeof(s_host)) {
            ssh_string_free_char(text);
            ssh_key_free(key);
            return false;
        }
        xSemaphoreTake(s_kmu, portMAX_DELAY);
        snprintf(s_host, sizeof(s_host), "%s", text);
        xSemaphoreGive(s_kmu);
        ssh_string_free_char(text);
        if (!save()) printf("term: ssh host key made but not saved\n");
    }
    unsigned char *hash = NULL;
    size_t hn = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hn) == SSH_OK) {
        char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hn);
        xSemaphoreTake(s_kmu, portMAX_DELAY);
        snprintf(s_fp, sizeof(s_fp), "%s", fp ? fp : "");
        xSemaphoreGive(s_kmu);
        ssh_string_free_char(fp);
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(key);
    return true;
}

static bool listen_now(void)
{
    /* No network up means no TCP/IP stack to bind on (as for telnet). */
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (!nif || !esp_netif_is_netif_up(nif)) return false;
    s_bind = ssh_bind_new();
    if (!s_bind) return false;
    int port = s_port;
    ssh_bind_options_set(s_bind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");
    ssh_bind_options_set(s_bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    if (ssh_bind_options_set(s_bind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, s_host) != SSH_OK ||
        ssh_bind_listen(s_bind) != SSH_OK) {
        unbind();
        return false;
    }
    s_listening = true;
    return true;
}

static void ssh_task(void *arg)
{
    (void)arg;
    if (ssh_init() != SSH_OK || !host_key_ready()) {
        printf("term: ssh could not start\n");
        s_task = NULL;
        vTaskDelete(NULL);
    }
    for (;;) {
        if (!s_enabled || s_rebind) {
            unbind();
            s_rebind = false;
            if (!s_enabled) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
        }
        if (!s_bind && !listen_now()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        const int fd = ssh_bind_get_fd(s_bind);
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = {.tv_usec = 300000};
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) { s_kick = false; continue; }
        ssh_session ses = ssh_new();
        if (!ses) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (ssh_bind_accept(s_bind, ses) == SSH_OK) serve(ses);
        ssh_free(ses);
    }
}

static bool task_start(void)
{
    if (s_task) return true;
    if (!rterm_ensure_started()) return false;
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stack = heap_caps_malloc(SSH_STACK, MALLOC_CAP_SPIRAM);
    if (tcb && stack) s_task = xTaskCreateStaticPinnedToCore(ssh_task, "ls_ssh", SSH_STACK, NULL, 2, stack, tcb, 0);
    if (!s_task) {
        heap_caps_free(tcb);
        heap_caps_free(stack);
        return false;
    }
    return true;
}

/* --- for ls_rterm.c ----------------------------------------------------- */

void rterm_ssh_load(void)
{
    if (!s_kmu) s_kmu = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        uint16_t port = SSH_PORT_DEFAULT;
        nvs_get_u8(h, "ssh", &on);
        nvs_get_u16(h, "sshport", &port);
        size_t len = sizeof(s_keys);
        if (nvs_get_str(h, "sshkeys", s_keys, &len) != ESP_OK) s_keys[0] = 0;
        len = sizeof(s_host);
        if (nvs_get_str(h, "sshhost", s_host, &len) != ESP_OK) s_host[0] = 0;
        nvs_close(h);
        s_enabled = on;
        s_port = port ? port : SSH_PORT_DEFAULT;
    }
    if (s_enabled && !task_start()) printf("term: no memory for ssh\n");
}

void rterm_ssh_status(ls_rterm_status_t *o)
{
    o->ssh_on = s_enabled;
    o->ssh_listening = s_listening;
    o->ssh_port = s_port;
    o->ssh_client = s_client;
    o->ssh_authed = s_authed;
    o->ssh_ip = s_client_ip;
    o->ssh_keys_max = KEYS_MAX;
    const int64_t left = s_learn_until - esp_timer_get_time();
    o->ssh_learn_s = left > 0 ? (int)(left / 1000000) + 1 : 0;
    if (!s_kmu) return;
    xSemaphoreTake(s_kmu, portMAX_DELAY);
    o->ssh_keys = count_lines(s_keys) + s_sd_keys;
    snprintf(o->ssh_fp, sizeof(o->ssh_fp), "%s", s_fp);
    xSemaphoreGive(s_kmu);
}

void rterm_ssh_kick(void) { if (s_client) s_kick = true; }

/* --- the public switches -------------------------------------------------- */

bool ls_rterm_ssh_set(bool on)
{
    if (!s_kmu) s_kmu = xSemaphoreCreateMutex();
    if (on && (!rterm_pass_ensure() || !task_start())) return false;
    s_enabled = on;
    return save();
}

bool ls_rterm_ssh_port_set(int port)
{
    if (port < 1 || port > 65535 || port == 2323) return false;   /* 2323 is telnet's */
    s_port = port;
    s_rebind = true;
    return save();
}

void ls_rterm_ssh_learn(bool on)
{
    s_learn_until = on ? esp_timer_get_time() + SSH_LEARN_US : 0;
}

bool ls_rterm_ssh_keys_clear(void)
{
    if (!s_kmu) return false;
    xSemaphoreTake(s_kmu, portMAX_DELAY);
    s_keys[0] = 0;
    xSemaphoreGive(s_kmu);
    return save();
}

/* --- console ------------------------------------------------------------- */

int rterm_ssh_console(int argc, char **argv)
{
    const char *what = argc >= 3 ? argv[2] : "";
    if (!strcmp(what, "on") || !strcmp(what, "off")) {
        if (!ls_rterm_ssh_set(what[1] == 'n')) { printf("term: could not switch ssh\n"); return 1; }
        if (what[1] == 'n')
            printf("term: ssh on, port %d (the host key is made on the first start)\n", s_port);
        else printf("term: ssh off\n");
        return 0;
    }
    if (!strcmp(what, "port") && argc >= 4) {
        if (!ls_rterm_ssh_port_set(atoi(argv[3]))) { printf("term: a port is 1-65535, not 2323\n"); return 1; }
        printf("term: ssh port %d\n", s_port);
        return 0;
    }
    if (!strcmp(what, "learn")) {
        ls_rterm_ssh_learn(true);
        printf("term: the key offered at the next password login is kept (2 minutes)\n");
        return 0;
    }
    if (!strcmp(what, "key") && argc >= 6 && !strcmp(argv[3], "add")) {
        char *line = malloc(KEY_LINE);
        if (line) snprintf(line, KEY_LINE, "%s %s %s", argv[4], argv[5], argc >= 7 ? argv[6] : "");
        const bool ok = line && s_kmu && key_add(line);
        free(line);
        if (!ok) { printf("term: not a key, or the list is full (%d)\n", KEYS_MAX); return 1; }
        printf("term: key added\n");
        return 0;
    }
    if (!strcmp(what, "key") && argc >= 5 && !strcmp(argv[3], "del")) {
        if (!s_kmu || !key_del(atoi(argv[4]) - 1)) { printf("term: no such key\n"); return 1; }
        printf("term: key removed\n");
        return 0;
    }
    if (!strcmp(what, "keys") && argc >= 4 && !strcmp(argv[3], "clear")) {
        ls_rterm_ssh_keys_clear();
        printf("term: keys cleared (the SD card's file is not touched)\n");
        return 0;
    }
    if (!strcmp(what, "keys")) {
        if (!s_kmu) { printf("term: no keys\n"); return 0; }
        xSemaphoreTake(s_kmu, portMAX_DELAY);
        int i = 0;
        for (const char *p = s_keys; *p; i++) {
            const size_t n = strcspn(p, "\n");
            const char *sp = memchr(p, ' ', n);
            const char *cm = sp ? memchr(sp + 1, ' ', n - (size_t)(sp + 1 - p)) : NULL;
            printf("%d %.*s ...%.*s\n", i + 1, sp ? (int)(sp - p) : (int)n, p,
                   cm ? (int)(n - (size_t)(cm - p)) : 0, cm ? cm : "");
            p += n + (p[n] ? 1 : 0);
        }
        xSemaphoreGive(s_kmu);
        printf("term: %d of %d in NVS; %s also read\n", i, KEYS_MAX, SD_KEYS);
        return 0;
    }
    if (what[0]) {
        printf("term: ssh on|off|port <n>|learn|keys [clear]|key add <type> <base64> [name]|key del <n>\n");
        return 1;
    }
    const uint32_t ip = ntohl(s_client_ip);
    printf("term: ssh %s on port %d, %s\n", s_enabled ? (s_listening ? "on" : "on, waiting for the network") : "off",
           s_port, !s_client ? "nobody connected" : s_authed ? "a client is in" : "a client is logging in");
    if (s_client)
        printf("term: ssh client %lu.%lu.%lu.%lu\n", (unsigned long)(ip >> 24), (unsigned long)(ip >> 16 & 255),
               (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255));
    if (s_fp[0]) printf("term: host key %s\n", s_fp);
    return 0;
}

#endif /* CONFIG_LS_SSH */
