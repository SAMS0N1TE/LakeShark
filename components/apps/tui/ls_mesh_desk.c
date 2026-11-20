/* See ls_mesh_desk.h. */
#include "ls_mesh_desk.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

uint32_t ls_meshd_mark(const ls_mesh_msg_t *m)
{
    if (m->id) return m->id;
    uint32_t h = (2166136261u ^ m->t) * 16777619u;
    h = (h ^ m->chan ^ (m->direct ? 0x100u : 0u)) * 16777619u;
    for (const char *c = m->peer; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    for (const char *c = m->text; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return h ? h : 1;
}

bool ls_meshd_belongs(const ls_mesh_msg_t *m, const ls_meshd_conv_t *c)
{
    if (c->kind == LS_MC_DM) return m->direct && !strcasecmp(m->peer, c->id);
    if (m->direct) return false;
    return m->chan == (c->kind == LS_MC_PUBLIC ? 0 : c->chan);
}

uint32_t ls_meshd_key(const ls_meshd_conv_t *c)
{
    if (c->kind != LS_MC_DM) return 0x80000000u | (c->kind == LS_MC_PUBLIC ? 0 : c->chan);
    uint32_t h = 2166136261u;
    for (const char *p = c->id; *p; p++) h = (h ^ (uint8_t)toupper((unsigned char)*p)) * 16777619u;
    return h & 0x7FFFFFFFu;
}

static const ls_meshd_read_one_t *read_of(const ls_meshd_read_t *read, uint32_t key)
{
    if (!read) return NULL;
    for (int i = 0; i < read->n; i++)
        if (read->e[i].key == key) return &read->e[i];
    return NULL;
}

int ls_meshd_unread(const ls_meshd_read_t *read, const ls_meshd_conv_t *c,
                    const ls_mesh_msg_t *msgs, int nmsgs)
{
    if (!read) return 0;
    const ls_meshd_read_one_t *r = read_of(read, ls_meshd_key(c));
    /* Counted back from the newest to the mark. A mark that has left the
       ring is older than everything in it. */
    int n = 0;
    for (int i = nmsgs - 1; i >= 0; i--) {
        if (!ls_meshd_belongs(&msgs[i], c)) continue;
        if (r && ls_meshd_mark(&msgs[i]) == r->mark) break;
        if (!msgs[i].mine) n++;
    }
    return n;
}

void ls_meshd_read_all(ls_meshd_read_t *read, const ls_meshd_conv_t *c,
                       const ls_mesh_msg_t *msgs, int nmsgs)
{
    int last = -1;
    for (int i = nmsgs - 1; i >= 0 && last < 0; i--)
        if (ls_meshd_belongs(&msgs[i], c)) last = i;
    const uint32_t key = ls_meshd_key(c), mark = last >= 0 ? ls_meshd_mark(&msgs[last]) : 0;
    for (int i = 0; i < read->n; i++)
        if (read->e[i].key == key) { read->e[i].mark = mark; return; }
    if (read->n >= LS_MESHD_READ) {
        /* Full: the oldest entry goes. Its conversation reads as unread
           again, which is the safe way to be wrong. */
        memmove(&read->e[0], &read->e[1], sizeof(read->e[0]) * (LS_MESHD_READ - 1));
        read->n = LS_MESHD_READ - 1;
    }
    read->e[read->n].key = key;
    read->e[read->n].mark = mark;
    read->n++;
}

static void tally(ls_meshd_conv_t *c, const ls_mesh_msg_t *msgs, int nmsgs, const ls_meshd_read_t *read)
{
    c->total = 0;
    c->last_t = 0;
    for (int i = 0; i < nmsgs; i++)
        if (ls_meshd_belongs(&msgs[i], c)) { c->total++; c->last_t = msgs[i].t; }
    c->unread = (uint16_t)ls_meshd_unread(read, c, msgs, nmsgs);
}

int ls_meshd_convs(ls_meshd_conv_t *out, int max,
                   const ls_mesh_msg_t *msgs, int nmsgs,
                   const ls_mesh_chan_t *chans, int nchans,
                   const ls_mesh_peer_t *peers, int npeers,
                   const ls_meshd_read_t *read)
{
    int n = 0;
    if (max < 1) return 0;
    memset(&out[n], 0, sizeof(out[n]));
    out[n].kind = LS_MC_PUBLIC;
    snprintf(out[n].name, sizeof(out[n].name), "%s", nchans > 0 && chans[0].name[0] ? chans[0].name : "Public");
    n++;
    for (int c = 1; c < nchans && n < max; c++) {
        if (!chans[c].psk[0]) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = LS_MC_CHANNEL;
        out[n].chan = (uint8_t)c;
        if (chans[c].name[0]) snprintf(out[n].name, sizeof(out[n].name), "%s", chans[c].name);
        else snprintf(out[n].name, sizeof(out[n].name), "Channel %d", c);
        n++;
    }
    /* A channel with messages and no key here (the key was taken out). */
    for (int i = 0; i < nmsgs && n < max; i++) {
        if (msgs[i].direct || !msgs[i].chan) continue;
        bool have = false;
        for (int k = 0; k < n; k++) have = have || (out[k].kind == LS_MC_CHANNEL && out[k].chan == msgs[i].chan);
        if (have) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = LS_MC_CHANNEL;
        out[n].chan = msgs[i].chan;
        snprintf(out[n].name, sizeof(out[n].name), "Channel %d", msgs[i].chan);
        n++;
    }
    const int first_dm = n;
    /* The nodes in the ring's DMs, the newest conversation first. */
    for (int i = nmsgs - 1; i >= 0 && n < max; i--) {
        if (!msgs[i].direct || !msgs[i].peer[0]) continue;
        bool have = false;
        for (int k = first_dm; k < n; k++) have = have || !strcasecmp(out[k].id, msgs[i].peer);
        if (have) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = LS_MC_DM;
        snprintf(out[n].id, sizeof(out[n].id), "%s", msgs[i].peer);
        n++;
    }
    for (int p = 0; p < npeers && n < max; p++) {
        bool have = false;
        for (int k = first_dm; k < n; k++) have = have || !strcasecmp(out[k].id, peers[p].id);
        if (have) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = LS_MC_DM;
        snprintf(out[n].id, sizeof(out[n].id), "%s", peers[p].id);
        n++;
    }
    for (int k = 0; k < n; k++) {
        if (out[k].kind == LS_MC_DM) {
            for (int p = 0; p < npeers; p++) {
                if (strcasecmp(peers[p].id, out[k].id)) continue;
                out[k].heard = true;
                out[k].role = peers[p].type;
                snprintf(out[k].name, sizeof(out[k].name), "%s", peers[p].name);
                break;
            }
            if (!out[k].name[0]) snprintf(out[k].name, sizeof(out[k].name), "%.8s", out[k].id);
        }
        tally(&out[k], msgs, nmsgs, read);
    }
    return n;
}

const char *ls_meshd_split(const ls_mesh_msg_t *m, char *who, size_t n)
{
    const char *colon = strstr(m->text, ": ");
    if (who && n) who[0] = 0;
    if (m->sender[0]) {
        if (who) snprintf(who, n, "%s", m->sender);
        /* The text may or may not still carry the name in front. */
        const size_t k = strlen(m->sender);
        if (!strncmp(m->text, m->sender, k) && m->text[k] == ':' && m->text[k + 1] == ' ') return m->text + k + 2;
        return m->text;
    }
    if (colon && colon - m->text < LS_MESH_PEER_NAME && colon > m->text) {
        if (who) snprintf(who, n, "%.*s", (int)(colon - m->text), m->text);
        return colon + 2;
    }
    return m->text;
}

const char *ls_meshd_state_word(const ls_mesh_msg_t *m)
{
    if (!m->mine) return "";
    switch (m->state) {
    case LS_MSG_QUEUED:  return "queued";
    case LS_MSG_SENDING: return "sending";
    case LS_MSG_SENT:    return "sent";
    case LS_MSG_HEARD:   return "relayed";
    case LS_MSG_ACKED:   return "delivered";
    case LS_MSG_FAILED:  return "failed";
    default:             return "sent";
    }
}

static bool holds(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

bool ls_meshd_match(const ls_mesh_msg_t *m, const char *needle)
{
    if (!needle || !*needle) return true;
    return holds(m->text, needle) || holds(m->sender, needle);
}

int ls_meshd_wrap(const char *text, int off, int width, char *line, size_t cap)
{
    const char *s = text + off;
    int n = 0, cells = 0;
    if (width < 1) width = 1;
    /* A cell a character: a UTF-8 tail byte takes none. */
    while (s[n] && s[n] != '\n') {
        if (((unsigned char)s[n] & 0xC0) != 0x80) { if (cells >= width) break; cells++; }
        n++;
    }
    if (s[n] && s[n] != '\n') {
        int space = n;
        while (space > 0 && s[space] != ' ') space--;
        if (space > 0) n = space;
    }
    if (line && cap) snprintf(line, cap, "%.*s", n, s);
    off += n;
    if (text[off] == '\n') off++;
    while (text[off] == ' ') off++;
    return off;
}

const char *ls_meshd_role(uint8_t type)
{
    switch (type) {
    case LS_MESH_ROLE_CHAT:     return "chat";
    case LS_MESH_ROLE_REPEATER: return "repeater";
    case LS_MESH_ROLE_ROOM:     return "room";
    case LS_MESH_ROLE_SENSOR:   return "sensor";
    default:                    return "-";
    }
}

/* Negative, zero, positive; `none` says one side has nothing to sort by. */
static int cmp(const ls_mesh_peer_t *a, const ls_mesh_peer_t *b, int col, float da, float db, int *none)
{
    *none = 0;
    switch (col) {
    case LS_MN_NAME: {
        const char *x = a->name[0] ? a->name : a->id, *y = b->name[0] ? b->name : b->id;
        if (!a->name[0] != !b->name[0]) { *none = a->name[0] ? -1 : 1; return 0; }
        return strcasecmp(x, y);
    }
    case LS_MN_ID: return strcasecmp(a->id, b->id);
    case LS_MN_ROLE: return strcmp(ls_meshd_role(a->type), ls_meshd_role(b->type));
    case LS_MN_HEARD:
        if (!a->last_heard != !b->last_heard) { *none = a->last_heard ? -1 : 1; return 0; }
        /* Ascending is the most recently heard first: the age grows. */
        return a->last_heard > b->last_heard ? -1 : a->last_heard < b->last_heard;
    case LS_MN_FIRST: return a->first_heard < b->first_heard ? -1 : a->first_heard > b->first_heard;
    case LS_MN_RSSI:
        if (!a->last_heard != !b->last_heard) { *none = a->last_heard ? -1 : 1; return 0; }
        return a->rssi > b->rssi ? -1 : a->rssi < b->rssi;
    case LS_MN_SNR:
        if (!a->last_heard != !b->last_heard) { *none = a->last_heard ? -1 : 1; return 0; }
        return a->snr > b->snr ? -1 : a->snr < b->snr;
    case LS_MN_DIST:
        if ((da < 0) != (db < 0)) { *none = da < 0 ? 1 : -1; return 0; }
        return da < db ? -1 : da > db;
    case LS_MN_ADVERTS: return a->adverts > b->adverts ? -1 : a->adverts < b->adverts;
    default: return 0;
    }
}

void ls_meshd_sort(uint8_t *order, const ls_mesh_peer_t *peers, int n, int col, bool desc, const float *dist_m)
{
    for (int i = 0; i < n; i++) order[i] = (uint8_t)i;
    /* An insertion sort: a dozen nodes, and it keeps equal ones in the order heard. */
    for (int i = 1; i < n; i++) {
        const uint8_t v = order[i];
        int k = i;
        while (k > 0) {
            const uint8_t u = order[k - 1];
            int none;
            int c = cmp(&peers[u], &peers[v], col, dist_m ? dist_m[u] : -1, dist_m ? dist_m[v] : -1, &none);
            if (none) c = none;
            else if (desc) c = -c;
            if (c <= 0) break;
            order[k] = u;
            k--;
        }
        order[k] = v;
    }
}

void ls_meshd_range_text(float metres, char *out, size_t n)
{
    if (metres < 0) snprintf(out, n, "-");
    else if (metres < 1000) snprintf(out, n, "%d m", (int)(metres + 0.5f));
    else if (metres < 100000) snprintf(out, n, "%.1f km", (double)metres / 1000.0);
    else snprintf(out, n, "%d km", (int)(metres / 1000.0f + 0.5f));
}

void ls_meshd_time_text(uint32_t t, char *out, size_t n)
{
    if (!t) { snprintf(out, n, "--:--"); return; }
    if (t >= 1000000000u) {
        const uint32_t s = t % 86400u;
        snprintf(out, n, "%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u));
        return;
    }
    /* No clock: seconds since the board started. */
    if (t < 3600) snprintf(out, n, "+%um%02u", (unsigned)(t / 60u), (unsigned)(t % 60u));
    else snprintf(out, n, "+%uh%02u", (unsigned)(t / 3600u), (unsigned)(t / 60u % 60u));
}
