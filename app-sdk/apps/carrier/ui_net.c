/* Other commanders over MeshCore. Off unless switched on in COMMS. When on,
 * the board says where you are in one short line on the chosen channel slot
 * every ten minutes (and when you arrive somewhere), and every CARRIER line
 * heard becomes a friendly ship near your station. Chat is quick replies.
 *
 * On air:  CARRIER Lanqu/a1f3 Pup o7
 *          (system / galaxy seed, rank, optional words) - readable by people
 *          in a normal MeshCore chat, parsed here. */
#include "internal.h"
#include "art.h"
#include "ui.h"

#define PEERS 6
#define LINES 16
static ui_peer peers[PEERS];
static int npeers;
static ui_line lines[LINES];
static int nlines;
static uint32_t seq_seen, last_announce_ms, unread;
static int last_system = -1;

int ui_net_count(void) { return npeers; }
const ui_peer *ui_net_peer(int i) { return i >= 0 && i < npeers ? &peers[i] : NULL; }
int ui_net_lines(void) { return nlines; }
const ui_line *ui_net_line(int i) { return i >= 0 && i < nlines ? &lines[i] : NULL; }
bool ui_net_unread(void) { return unread > 0; }
void ui_net_mark_read(void) { unread = 0; }

bool ui_net_tx_ok(void) {
    lap_mesh_self_t me;
    return lap_mesh_self(&me) && me.running && me.tx_armed;
}

static bool send(const char *words) {
    const game_view *v = game_state();
    if (!ui.mesh_on || !v) return false;
    char out[150];
    snprintf(out, sizeof out, "CARRIER %s/%04x %s%s%s", v->systems[v->system].name,
             (unsigned)(v->galaxy_seed & 0xFFFF), game_rank(v->player.rank), words && *words ? " " : "",
             words ? words : "");
    return lap_mesh_send(ui.mesh_slot, out);
}
void ui_net_announce(void) {
    send(NULL);
    last_announce_ms = lap_millis();
}
bool ui_net_say(const char *words) { return send(words); }

/* Where a peer's ship waits: a few hundred units off the station, its own spot. */
static l3_vec spot(uint32_t h) {
    const game_view *v = game_state();
    float a = (h & 1023) / 1023.f * 2 * CG_PI, r = 260 + (h >> 10 & 255);
    return l3_add(v->contents.station, l3v(cosf(a) * r, -40 + (float)(h >> 18 & 63), sinf(a) * r));
}

static void heard(const lap_mesh_msg_t *m) {
    /* "name: CARRIER System/abcd Rank words" */
    const char *t = strstr(m->text, "CARRIER ");
    if (nlines == LINES) { memmove(lines, lines + 1, sizeof lines[0] * (LINES - 1)); nlines--; }
    ui_line *l = &lines[nlines++];
    memset(l, 0, sizeof *l);
    l->mine = m->mine;
    l->carrier = t != NULL;
    if (t) {
        char sys[24] = "", rank[20] = "";
        unsigned seed = 0;
        int used = 0;
        /* by hand: no sscanf for apps */
        const char *q = t + 8, *slash = strchr(q, '/');
        bool ok = slash && slash - q < (int)sizeof sys;
        if (ok) {
            memcpy(sys, q, (size_t)(slash - q));
            sys[slash - q] = 0;
            q = slash + 1;
            for (int k = 0; k < 4 && *q; k++, q++) {
                int h = *q >= '0' && *q <= '9' ? *q - '0' : (*q | 32) >= 'a' && (*q | 32) <= 'f' ? (*q | 32) - 'a' + 10 : -1;
                if (h < 0) { ok = false; break; }
                seed = seed * 16 + (unsigned)h;
            }
            while (*q == ' ') q++;
            int r = 0;
            while (*q && *q != ' ' && r < (int)sizeof rank - 1) rank[r++] = *q++;
            rank[r] = 0;
            while (*q == ' ') q++;
            used = (int)(q - (t + 8));
        }
        if (ok) {
            const char *words = t + 8 + used;
            snprintf(l->text, sizeof l->text, "%.12s %s%s%s", m->sender[0] ? m->sender : "?",
                     words[0] ? words : "is at ", words[0] ? "" : sys, "");
            if (!m->mine) {
                int i = 0;
                for (; i < npeers; i++) if (!strcmp(peers[i].id, m->peer[0] ? m->peer : m->sender)) break;
                if (i == npeers && npeers < PEERS) npeers++;
                if (i < npeers) {
                    ui_peer *p = &peers[i];
                    snprintf(p->id, sizeof p->id, "%s", m->peer[0] ? m->peer : m->sender);
                    snprintf(p->name, sizeof p->name, "%.15s", m->sender[0] ? m->sender : "Commander");
                    snprintf(p->system, sizeof p->system, "%s", sys);
                    snprintf(p->rank, sizeof p->rank, "%s", rank);
                    p->same_galaxy = (seed & 0xFFFF) == (game_state()->galaxy_seed & 0xFFFF);
                    p->heard_ms = lap_millis();
                    p->position = spot(cg_hash((uint32_t)i * 2654435761u ^ (uint32_t)strlen(p->id) ^
                                               (uint32_t)p->name[0] << 8));
                }
                unread++;
            }
            return;
        }
    }
    snprintf(l->text, sizeof l->text, "%s", m->text);
    if (!m->mine) unread++;
}

/* Each frame: cheap unless the mesh's message sequence moved. */
void ui_net_update(void) {
    const game_view *v = game_state();
    if (!ui.mesh_on || !v) return;
    uint32_t seq = lap_mesh_seq();
    if (seq != seq_seen) {
        static lap_mesh_msg_t m[24];
        int n = lap_mesh_messages(m, 24);
        static uint32_t newest;
        for (int i = 0; i < n; i++)
            if (m[i].id > newest && m[i].channel == ui.mesh_slot && !m[i].direct) heard(&m[i]);
        for (int i = 0; i < n; i++) if (m[i].id > newest) newest = m[i].id;
        seq_seen = seq;
    }
    /* forget peers not heard for half an hour */
    for (int i = 0; i < npeers; i++)
        if (lap_millis() - peers[i].heard_ms > 30u * 60 * 1000) {
            peers[i] = peers[--npeers];
            i--;
        }
    /* say where you are: on arrival, and every ten minutes */
    bool moved = last_system != v->system && v->jump == JUMP_IDLE;
    if ((moved || lap_millis() - last_announce_ms > 10u * 60 * 1000) && ui_net_tx_ok()) {
        ui_net_announce();
        last_system = v->system;
    }
    /* peers drift gently around their spot */
    for (int i = 0; i < npeers; i++) {
        float a = v->clock * .05f + i;
        peers[i].attitude = l3_quat_euler(a / CG_RAD, 0, 0);
    }
}

/* Commanders on the mesh: friendly Makos idling near the station, names above. */
void render_peers(l3_scene *sc) {
    for (int i = 0; i < npeers; i++) {
        const ui_peer *pe = &peers[i];
        l3_tag(sc, 200 + i);
        l3_xform t = l3_pose(pe->position, pe->attitude, 6);
        l3_draw_mesh(sc, game_mesh(SH_COBRA), &t, render_material(1));
    }
    l3_tag(sc, 0);
}
