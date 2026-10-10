/* Serialized saved-network NVS operations; included by firmware and host tests. */
#define WIFI_STORE_MAGIC UINT32_C(0x8A25C7FC)
#define WIFI_STORE_BUILD UINT32_C(0xF60A5E45)

static esp_err_t store_header(nvs_handle_t h) {
    esp_err_t e=nvs_set_u32(h,"magic",WIFI_STORE_MAGIC);
    if(e==ESP_OK) e=nvs_set_u32(h,"build",WIFI_STORE_BUILD);
    return e;
}

/* All functions touching NVS run inside store_job via ls_nvs_run. */
typedef struct {
    int op;                         /* 0 read, 1 upsert, 2 forget, 3 password */
    const char *ssid, *pass;
    char *password;
} store_job_t;

static void slot_key(char key[3], char prefix, int slot)
{
    key[0] = prefix; key[1] = '0' + slot; key[2] = 0;
}

static esp_err_t read_slots(nvs_handle_t h)
{
    memset(s_saved, 0, sizeof(s_saved));
    uint32_t magic=0, build=0;
    esp_err_t e=nvs_get_u32(h,"magic",&magic);
    if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) return e;
    e=nvs_get_u32(h,"build",&build);
    if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) return e;
    if(magic!=WIFI_STORE_MAGIC || build!=WIFI_STORE_BUILD) return ESP_OK;
    for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i) {
        char key[3]; size_t n = sizeof(s_saved[i].ssid);
        slot_key(key, 's', i);
        esp_err_t e = nvs_get_str(h, key, s_saved[i].ssid, &n);
        if (e == ESP_ERR_NVS_NOT_FOUND) continue;
        if (e != ESP_OK) return e;
        slot_key(key, 'q', i);
        e = nvs_get_u32(h, key, &s_saved[i].sequence);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) return e;
    }
    return ESP_OK;
}

static esp_err_t write_slot(nvs_handle_t h, const char *ssid, const char *pass)
{
    uint32_t magic=0, build=0;
    esp_err_t header_error=nvs_get_u32(h,"magic",&magic);
    if(header_error!=ESP_OK && header_error!=ESP_ERR_NVS_NOT_FOUND) return header_error;
    header_error=nvs_get_u32(h,"build",&build);
    if(header_error!=ESP_OK && header_error!=ESP_ERR_NVS_NOT_FOUND) return header_error;
    if(magic!=WIFI_STORE_MAGIC || build!=WIFI_STORE_BUILD) {
        for(int i=0;i<LS_WIFI_SAVED_MAX;i++) for(int k=0;k<3;k++) {
            char key[3];slot_key(key,"spq"[k],i);
            esp_err_t e=nvs_erase_key(h,key);
            if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) return e;
        }
    }
    int slot = ls_wifi_saved_slot(s_saved, LS_WIFI_SAVED_MAX, ssid);
    uint32_t newest = 0;
    for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i)
        if (s_saved[i].sequence > newest) newest = s_saved[i].sequence;
    /* Renumber before u32 overflow, preserving order (including ties). */
    if (newest == UINT32_MAX) {
        uint32_t ranks[LS_WIFI_SAVED_MAX] = {0};
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i) {
            if (!s_saved[i].ssid[0]) continue;
            ranks[i] = 1;
            for (int j = 0; j < LS_WIFI_SAVED_MAX; ++j)
                if (s_saved[j].ssid[0] && (s_saved[j].sequence < s_saved[i].sequence ||
                    (s_saved[j].sequence == s_saved[i].sequence && j < i))) ranks[i]++;
        }
        newest = 0;
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i) {
            if (!ranks[i]) continue;
            char key[3]; slot_key(key, 'q', i);
            esp_err_t e = nvs_set_u32(h, key, ranks[i]);
            if (e != ESP_OK) return e;
            s_saved[i].sequence = ranks[i];
            if (ranks[i] > newest) newest = ranks[i];
        }
    }
    char key[3]; slot_key(key, 'p', slot);
    esp_err_t e = nvs_set_str(h, key, pass);
    if (e != ESP_OK) return e;
    slot_key(key, 'q', slot);
    e = nvs_set_u32(h, key, newest + 1);
    if (e != ESP_OK) return e;
    slot_key(key, 's', slot);
    e = nvs_set_str(h, key, ssid);
    if (e != ESP_OK) return e;
    e=store_header(h);
    return e==ESP_OK ? nvs_commit(h) : e;
}

static esp_err_t erase_key(nvs_handle_t h, const char *key)
{
    esp_err_t e = nvs_erase_key(h, key);
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
}

static esp_err_t store_job(void *ctx)
{
    store_job_t *j = ctx;
    nvs_handle_t h;
    esp_err_t e = nvs_open_creds(NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = read_slots(h);
    if (e != ESP_OK) goto out;
    /* Commit the migrated slot BEFORE deleting legacy keys. A failed write
     * leaves the original credentials available for the next read. */
    char legacy[33] = {0}, password[64] = {0};
    size_t n = sizeof(legacy);
    e = nvs_get_str(h, LS_WIFI_NVS_KEY_SSID, legacy, &n);
    if (e == ESP_OK) {
        n = sizeof(password);
        e = nvs_get_str(h, LS_WIFI_NVS_KEY_PASS, password, &n);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        bool exists = false;
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i)
            if (!strcmp(s_saved[i].ssid, legacy)) exists = true;
        if (e == ESP_OK && !exists) e = write_slot(h, legacy, password);
        wipe(password, sizeof(password));
        if (e != ESP_OK) goto out;
        e = erase_key(h, LS_WIFI_NVS_KEY_SSID);
        if (e == ESP_OK) e = erase_key(h, LS_WIFI_NVS_KEY_PASS);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e == ESP_OK) e = read_slots(h);
    } else if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e != ESP_OK) goto out;
    if (j->op == 1) e = write_slot(h, j->ssid, j->pass);
    else if (j->op == 2) {
        bool found = false;
        for (int i = 0; i < LS_WIFI_SAVED_MAX && e == ESP_OK; ++i) {
            if (j->ssid && strcmp(s_saved[i].ssid, j->ssid)) continue;
            if (!s_saved[i].ssid[0]) continue;
            found = true;
            const char prefixes[] = "spq";
            for (int k = 0; k < 3 && e == ESP_OK; ++k) {
                char key[3]; slot_key(key, prefixes[k], i);
                e = erase_key(h, key);
            }
        }
        if (e == ESP_OK) e = nvs_commit(h);
        if (e == ESP_OK && j->ssid && !found) e = ESP_ERR_NOT_FOUND;
    } else if (j->op == 3) {
        e = ESP_ERR_NOT_FOUND;
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i) {
            if (strcmp(s_saved[i].ssid, j->ssid)) continue;
            char key[3]; slot_key(key, 'p', i); n = 64;
            e = nvs_get_str(h, key, j->password, &n);
            if (e == ESP_ERR_NVS_NOT_FOUND) { j->password[0] = 0; e = ESP_OK; }
            break;
        }
    }
out:
    /* Refresh the RAM snapshot only after an explicit mutation. Polling must
     * not reopen NVS to discover a write made by this same serialized owner. */
    if (e == ESP_OK && (j->op == 1 || j->op == 2)) e = read_slots(h);
    nvs_close(h);
    return e;
}

