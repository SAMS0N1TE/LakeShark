#include "scan_channels.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char  *TAG    = "scanch";
static const char  *NS     = "sdr-tool";
static const char  *BLOBK  = "scanlist";

static scan_channel_t s_ch[SCAN_MAX_CHANNELS];
static int            s_count = 0;
static nvs_handle_t   s_nvs   = 0;
static bool           s_ok    = false;

void scan_channels_init(void)
{
    s_count = 0;
    memset(s_ch, 0, sizeof(s_ch));

    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed");
        return;
    }
    s_ok = true;

    size_t sz = sizeof(s_ch);
    esp_err_t err = nvs_get_blob(s_nvs, BLOBK, s_ch, &sz);
    if (err == ESP_OK) {
        int n = (int)(sz / sizeof(scan_channel_t));
        if (n > SCAN_MAX_CHANNELS) n = SCAN_MAX_CHANNELS;
        s_count = n;
        ESP_LOGI(TAG, "loaded %d channels", s_count);
    } else {
        ESP_LOGI(TAG, "no stored channel list");
    }
}

int scan_channels_count(void) { return s_count; }

const scan_channel_t *scan_channel_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_ch[idx];
}

bool scan_channels_save(void)
{
    if (!s_ok) return false;
    esp_err_t err = nvs_set_blob(s_nvs, BLOBK, s_ch,
                                 (size_t)s_count * sizeof(scan_channel_t));
    if (err != ESP_OK) { ESP_LOGW(TAG, "save blob: %d", err); return false; }
    return nvs_commit(s_nvs) == ESP_OK;
}

int scan_channel_add(const char *name, uint32_t freq_hz, scan_mode_t mode, uint8_t zone)
{
    if (s_count >= SCAN_MAX_CHANNELS) return -1;
    if (freq_hz < 1000000UL || freq_hz > 2000000000UL) return -1;
    if (mode > SCAN_MODE_WFM) return -1;
    if (zone >= SCAN_MAX_ZONES) zone = 0;

    scan_channel_t *c = &s_ch[s_count];
    memset(c, 0, sizeof(*c));
    if (name && *name) {
        strncpy(c->name, name, SCAN_NAME_LEN - 1);
        c->name[SCAN_NAME_LEN - 1] = 0;
    } else {
        snprintf(c->name, SCAN_NAME_LEN, "CH%d", s_count + 1);
    }
    c->freq_hz = freq_hz;
    c->mode    = (uint8_t)mode;
    c->zone    = zone;
    c->flags   = SCAN_FLAG_ENABLED;

    int idx = s_count++;
    scan_channels_save();
    return idx;
}

bool scan_channel_remove(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    for (int i = idx; i < s_count - 1; i++) s_ch[i] = s_ch[i + 1];
    s_count--;
    memset(&s_ch[s_count], 0, sizeof(s_ch[s_count]));
    scan_channels_save();
    return true;
}

static bool set_flag(int idx, uint8_t flag, bool on)
{
    if (idx < 0 || idx >= s_count) return false;
    if (on) s_ch[idx].flags |=  flag;
    else    s_ch[idx].flags &= ~flag;
    scan_channels_save();
    return true;
}

bool scan_channel_set_lockout(int idx, bool on)  { return set_flag(idx, SCAN_FLAG_LOCKOUT,  on); }
bool scan_channel_set_priority(int idx, bool on) { return set_flag(idx, SCAN_FLAG_PRIORITY, on); }
bool scan_channel_set_enabled(int idx, bool on)  { return set_flag(idx, SCAN_FLAG_ENABLED,  on); }

void scan_channels_clear(void)
{
    s_count = 0;
    memset(s_ch, 0, sizeof(s_ch));
    scan_channels_save();
}

const char *scan_mode_name(uint8_t mode)
{
    switch (mode) {
        case SCAN_MODE_P25: return "P25";
        case SCAN_MODE_NFM: return "NFM";
        case SCAN_MODE_WFM: return "WFM";
        default:            return "?";
    }
}

int scan_mode_parse(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "p25")) return SCAN_MODE_P25;
    if (!strcasecmp(s, "nfm") || !strcasecmp(s, "fm")) return SCAN_MODE_NFM;
    if (!strcasecmp(s, "wfm")) return SCAN_MODE_WFM;
    return -1;
}
