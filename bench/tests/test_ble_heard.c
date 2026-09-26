#include "ls_test.h"
#include "ls_ble_heard.h"
#include <string.h>

static uint8_t mac(int i, uint8_t out[6]) { memset(out, 0, 6); out[5] = (uint8_t)i; return out[5]; }

LS_CASE(devices_are_listed_strongest_first_and_keep_their_name)
{
    ls_ble_heard_clear();
    uint8_t a[6], b[6];
    mac(1, a); mac(2, b);
    ls_ble_heard_note(a, "Tag", 3, -80, 1000);
    ls_ble_heard_note(b, NULL, 0, -50, 1000);
    ls_ble_heard_note(a, NULL, 0, -78, 2000);             /* no name this time */
    ls_ble_heard_t list[4];
    LS_EQ_INT(ls_ble_heard_list(list, 4, 2000, 10000000), 2);
    LS_EQ_INT(list[0].addr[5], 2);
    LS_EQ_STR(list[1].name, "Tag");
    LS_EQ_INT(list[1].rssi, -78);
    LS_EQ_UINT(list[1].adverts, 2);
}

LS_CASE(stale_devices_drop_out_and_a_full_table_reuses_the_oldest)
{
    ls_ble_heard_clear();
    uint8_t m[6];
    for (int i = 0; i < LS_BLE_HEARD_MAX; i++) { mac(i, m); ls_ble_heard_note(m, NULL, 0, -60, 1000 + i); }
    mac(200, m); ls_ble_heard_note(m, "new", 3, -40, 5000);
    ls_ble_heard_t d;
    mac(0, m); LS_CHECK(!ls_ble_heard_find(m, &d));    /* the oldest went */
    mac(200, m); LS_CHECK(ls_ble_heard_find(m, &d));
    ls_ble_heard_t list[LS_BLE_HEARD_MAX];
    LS_EQ_INT(ls_ble_heard_list(list, LS_BLE_HEARD_MAX, 5000, 100), 1);  /* only the fresh one */
}
