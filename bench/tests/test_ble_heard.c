#include "ls_test.h"
#include "ls_ble_heard.h"
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_uint ready;
static atomic_bool go;
static void *first_note(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go)) { }
    uint8_t addr[6] = {0}; addr[5] = (uint8_t)id;
    for (unsigned i = 0; i < 100; ++i) ls_ble_heard_note(addr, "first", 5, -60, i);
    return NULL;
}


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

LS_CASE(concurrent_first_use_keeps_both_mutex_owners_and_all_adverts)
{
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, first_note, (void *)(uintptr_t)1);
    pthread_create(&threads[1], NULL, first_note, (void *)(uintptr_t)2);
    while (atomic_load(&ready) != 2) { }
    atomic_store(&go, true);
    pthread_join(threads[0], NULL); pthread_join(threads[1], NULL);
    ls_ble_heard_t list[2]; LS_EQ_INT(ls_ble_heard_list(list, 2, 100, 1000), 2);
    LS_EQ_UINT(list[0].adverts, 100); LS_EQ_UINT(list[1].adverts, 100);
}
