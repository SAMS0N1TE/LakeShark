/* Screen-owned journal and cargo ledger; no commander-save format changes. */
#ifndef CARRIER_UI_JOURNAL_H
#define CARRIER_UI_JOURNAL_H
enum { JOURNAL_TRADE, JOURNAL_COMBAT, JOURNAL_MISSION, JOURNAL_NAV, JOURNAL_COMMS, JOURNAL_KINDS };
typedef struct {
    uint32_t day, jump;
    uint16_t minute;
    int16_t system;
    uint8_t kind;
    char words[79];
} ui_journal_entry;
void ui_journal_reset(void);
void ui_journal_update(void);
void ui_journal_flush(void);
void ui_journal_add(int kind, int system, const char *words);
int ui_journal_count(void);
const ui_journal_entry *ui_journal_get(int newest);
bool ui_cargo_cost(int good, uint32_t *paid);
bool ui_cargo_trade(int good, int amount, bool buy);
void ui_panel_layout(void);
#endif
