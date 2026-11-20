/* A bounded journal watches screen-visible state edges, including while flying.
 * Purchases through MARKET record the exact transaction, other acquisitions
 * have no assumed purchase price. A small cache file survives commander reloads. */
#include "internal.h"
#include "ui.h"
#include "ui_journal.h"

#define JOURNAL_MAX 56
static struct {
    uint32_t magic, seed, sum, jump, credits;
    uint16_t count, head, qty[GAME_GOODS], known[GAME_GOODS];
    uint32_t cost[GAME_GOODS];
    ui_journal_entry entries[JOURNAL_MAX];
} book;
_Static_assert(sizeof book <= 8192, "journal exceeds cache file");
static struct {
    bool ready, dirty, docked;
    int system, messages, net;
    uint32_t kills, saved_ms;
    uint8_t missions[6];
    char latest[80], net_latest[60];
} seen;

static uint32_t book_sum(void) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)&book;
    for (size_t i = 12; i < sizeof book; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
void ui_journal_reset(void) { memset(&seen, 0, sizeof seen); }
int ui_journal_count(void) { return book.count; }
const ui_journal_entry *ui_journal_get(int newest) {
    if (newest < 0 || newest >= book.count) return NULL;
    return &book.entries[(book.head + JOURNAL_MAX - 1 - newest) % JOURNAL_MAX];
}
void ui_journal_add(int kind, int system, const char *words) {
    const game_view *v = game_state();
    if (!v || kind < 0 || kind >= JOURNAL_KINDS) return;
    ui_journal_entry *e = &book.entries[book.head];
    memset(e, 0, sizeof *e);
    lap_time_t tm = {0}; lap_time(&tm);
    e->day = v->market_day; e->jump = book.jump;
    e->minute = (uint16_t)(tm.hour * 60 + tm.minute);
    e->system = (int16_t)system; e->kind = (uint8_t)kind;
    snprintf(e->words, sizeof e->words, "%s", words);
    book.head = (book.head + 1) % JOURNAL_MAX;
    if (book.count < JOURNAL_MAX) book.count++;
    if (ui.log_scroll > 0) ui.log_scroll++;
    seen.dirty = true;
}
bool ui_cargo_cost(int good, uint32_t *paid) {
    if (good < 0 || good >= GAME_GOODS || book.known[good] != book.qty[good]) return false;
    *paid = book.cost[good]; return true;
}
static uint8_t mission_state(const game_mission *m) {
    return m->failed ? 3 : m->done ? 2 : m->accepted ? 1 : 0;
}
void ui_journal_update(void) {
    const game_view *v = game_state();
    if (!v) return;
    const game_player *p = &v->player;
    char words[96];
    if (!seen.ready || book.seed != v->galaxy_seed) {
        memset(&book, 0, sizeof book);
        int fd = lap_file_open("journal.bin", "r");
        int n = fd >= 0 ? lap_file_read(fd, &book, sizeof book) : 0;
        if (fd >= 0) lap_file_close(fd);
        if (n != sizeof book || book.magic != 0x314c4e4a || book.seed != v->galaxy_seed ||
            book.sum != book_sum() || book.count > JOURNAL_MAX || book.head >= JOURNAL_MAX) {
            memset(&book, 0, sizeof book);
            book.magic = 0x314c4e4a; book.seed = v->galaxy_seed;
        }
        for (int i = 0; i < GAME_GOODS; i++) if (book.credits != p->credits || book.qty[i] != p->cargo[i]) {
            book.qty[i] = p->cargo[i]; book.known[i] = 0; book.cost[i] = 0;
        }
        seen.ready = true; seen.system = v->system; seen.docked = p->docked;
        seen.kills = p->kills; seen.messages = v->message_count;
        snprintf(seen.latest, sizeof seen.latest, "%s", v->message_count ? v->messages[0] : "");
        for (int i = 0; i < 6; i++) seen.missions[i] = mission_state(&v->missions[i]);
        snprintf(words, sizeof words, "Commander aboard at %s", v->systems[v->system].name);
        ui_journal_add(JOURNAL_NAV, v->system, words);
    }
    if (seen.system != v->system) {
        book.jump++;
        snprintf(words, sizeof words, "Arrived at %s", v->systems[v->system].name);
        ui_journal_add(JOURNAL_NAV, v->system, words); seen.system = v->system;
    }
    if (seen.docked != p->docked) {
        snprintf(words, sizeof words, "%s %s", p->docked ? "Docked at" : "Launched from", v->systems[v->system].name);
        ui_journal_add(JOURNAL_NAV, v->system, words); seen.docked = p->docked;
    }
    if (p->kills > seen.kills) {
        snprintf(words, sizeof words, "Destroyed %u hostile ship%s", (unsigned)(p->kills - seen.kills), p->kills - seen.kills == 1 ? "" : "s");
        ui_journal_add(JOURNAL_COMBAT, v->system, words);
    }
    seen.kills = p->kills;
    for (int i = 0; i < 6; i++) {
        const game_mission *m = &v->missions[i]; uint8_t state = mission_state(m);
        if (state && state != seen.missions[i]) {
            snprintf(words, sizeof words, "%s: %s", state == 3 ? "Failed" : state == 2 ? "Completed" : "Accepted", m->title);
            ui_journal_add(JOURNAL_MISSION, m->destination, words);
        }
        seen.missions[i] = state;
    }
    for (int i = 0; i < GAME_GOODS; i++) {
        int delta = p->cargo[i] - book.qty[i];
        if (!delta) continue;
        snprintf(words, sizeof words, "%s %d t %s", delta > 0 ? "Received" : "Removed", abs(delta), v->market[i].name);
        ui_journal_add(JOURNAL_TRADE, v->system, words);
        if (delta < 0 && book.qty[i]) {
            book.cost[i] = (uint32_t)((uint64_t)book.cost[i] * p->cargo[i] / book.qty[i]);
            /* With mixed known/unknown cargo, removal cannot establish which
               units remain. Keep the remaining cost unknown conservatively. */
            int removed = -delta;
            book.known[i] = book.known[i] > removed ? book.known[i] - removed : 0;
        }
        book.qty[i] = p->cargo[i];
    }
    /* Find the overlap in the game's newest-first message queue, even when full. */
    if (v->message_count && (seen.messages != v->message_count || strcmp(seen.latest, v->messages[0]))) {
        int fresh = 0;
        while (fresh < v->message_count && strcmp(seen.latest, v->messages[fresh])) fresh++;
        for (int i = fresh - 1; i >= 0; i--) {
            int kind = JOURNAL_COMMS;
            const char *s = v->messages[i];
            if (strstr(s, "Missile") || strstr(s, "shield") || strstr(s, "attack") || strstr(s, "destroy")) kind = JOURNAL_COMBAT;
            else if (strstr(s, "Mission") || strstr(s, "mission") || strstr(s, "Delivery")) kind = JOURNAL_MISSION;
            else if (strstr(s, "Jump") || strstr(s, "jump") || strstr(s, "Dock") || strstr(s, "Cruise")) kind = JOURNAL_NAV;
            else if (strstr(s, "cargo") || strstr(s, "Cargo") || strstr(s, "trade")) kind = JOURNAL_TRADE;
            ui_journal_add(kind, v->system, s);
        }
    }
    seen.messages = v->message_count;
    snprintf(seen.latest, sizeof seen.latest, "%s", v->message_count ? v->messages[0] : "");
    int net = ui_net_lines();
    const ui_line *last = net ? ui_net_line(net - 1) : NULL;
    if (last && (net != seen.net || strcmp(seen.net_latest, last->text))) {
        int first = 0;
        if (seen.net) for (int i = net - 1; i >= 0; i--) {
            const ui_line *s = ui_net_line(i);
            if (s && !strcmp(s->text, seen.net_latest)) { first = i + 1; break; }
        }
        for (int i = first; i < net; i++) {
            const ui_line *s = ui_net_line(i);
            if (s) ui_journal_add(JOURNAL_COMMS, v->system, s->text);
        }
        snprintf(seen.net_latest, sizeof seen.net_latest, "%s", last->text);
    }
    seen.net = net;
    if (seen.dirty && lap_millis() - seen.saved_ms > 1000) {
        book.credits = p->credits; book.sum = book_sum();
        int fd = lap_file_open("journal.bin", "w");
        if (fd >= 0) { seen.dirty = lap_file_write(fd, &book, sizeof book) != sizeof book; lap_file_close(fd); }
        seen.saved_ms = lap_millis();
    }
}
void ui_journal_flush(void) {
    if (!game_state()) return;
    ui_journal_update();
    if (!seen.dirty) return;
    book.credits = game_state()->player.credits; book.sum = book_sum();
    int fd = lap_file_open("journal.bin", "w");
    if (fd >= 0) { seen.dirty = lap_file_write(fd, &book, sizeof book) != sizeof book; lap_file_close(fd); }
}
bool ui_cargo_trade(int good, int amount, bool buy) {
    ui_journal_update();
    if (good < 0 || good >= GAME_GOODS) return false;
    const game_view *v = game_state();
    if (!v) return false;
    uint32_t total = (uint32_t)(amount > 0 ? amount : 0) * v->market[good].price;
    if (!(buy ? game_market_buy(good, amount) : game_market_sell(good, amount))) return false;
    if (buy) { book.cost[good] += total; book.known[good] += amount; }
    else if (book.qty[good]) {
        book.cost[good] = (uint32_t)((uint64_t)book.cost[good] * (book.qty[good] - amount) / book.qty[good]);
        book.known[good] = book.known[good] > amount ? book.known[good] - amount : 0;
    }
    book.qty[good] = v->player.cargo[good];
    char words[96];
    snprintf(words, sizeof words, "%s %d t %s for %u cr", buy ? "Bought" : "Sold", amount, v->market[good].name, (unsigned)total);
    ui_journal_add(JOURNAL_TRADE, v->system, words);
    return true;
}
