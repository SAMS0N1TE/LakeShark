/* Production port bookkeeping, with radio and task scheduling mocked. */
extern "C" {
#include "ls_test.h"
}
extern "C" {
#include "esp_timer.h"
#include "esp_random.h"
}
#include "freertos/task.h"
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <mutex>
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
static int mesh_test_mkdir(const char *, int) { return 0; }
#define mkdir(path, mode) mesh_test_mkdir(path, mode)
static std::recursive_mutex mesh_test_mutex;
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(mux) mesh_test_mutex.lock()
#define portEXIT_CRITICAL(mux) mesh_test_mutex.unlock()
#include "../../components/meshcore/ls_mesh.cpp"
#undef mkdir


extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }

#ifdef _WIN32
#include <process.h>
/* mingw's tmpfile() writes to C:\ and fails without admin rights */
static FILE *host_tmpfile(void)
{
    static unsigned n;
    char path[300];
    const char *dir = getenv("TEMP");
    snprintf(path, sizeof(path), "%s/lsmesh%d_%u.tmp", dir ? dir : ".", _getpid(), n++);
    return fopen(path, "w+bD");
}
#define tmpfile host_tmpfile
#endif

LS_CASE(two_queued_packets_have_independent_delivery)
{
    int a = note_msg("first", true, 0, NULL);
    int b = note_msg("second", true, 0, NULL);
    mesh::Packet pa, pb;
    delivery_bind(&pa, a); delivery_bind(&pb, b);
    delivery_state(&pa, LS_MSG_SENDING);
    LS_EQ_INT(s_msgs[b].state, LS_MSG_QUEUED);
    delivery_state(&pa, LS_MSG_SENT); delivery_free(&pa);
    LS_EQ_INT(s_msgs[a].state, LS_MSG_SENT);
    LS_EQ_INT(s_msgs[b].state, LS_MSG_QUEUED);
    delivery_free(&pb);
    LS_EQ_INT(s_msgs[b].state, LS_MSG_FAILED);
}

LS_CASE(ring_overwrite_invalidates_ack_echo_and_delivery)
{
    int slot = note_msg("old", true, 0, "0123456789ABCDEF");
    mesh::Packet packet; delivery_bind(&packet, slot);
    s_ack[0] = {123, slot, true}; s_sent[0].slot = slot; s_sent[0].used = true;
    for (int i = 0; i < LS_MESH_MAX_MSGS; i++) note_msg("replacement", true, 0, NULL);
    LS_CHECK(!s_ack[0].used); LS_CHECK(!s_sent[0].used);
    delivery_state(&packet, LS_MSG_SENT);
    LS_EQ_INT(s_msgs[slot].state, LS_MSG_QUEUED);
}

LS_CASE(protocol_sized_history_and_sender_metadata)
{
    char text[180]; memset(text, 'x', sizeof(text) - 1); text[179] = 0;
    int slot = note_msg(text, false, 0, "0123456789ABCDEF");
    snprintf(s_msgs[slot].sender, sizeof(s_msgs[slot].sender), "sender");
    LS_EQ_INT(strlen(s_msgs[slot].text), 179);
    LS_EQ_STR(s_msgs[slot].sender, "sender");
}

LS_CASE(pool_drain_returns_all_packets_and_frees_storage)
{
    unsigned before = ls_shim_heap_outstanding();
    {
        LsPoolManager pool(4); LS_CHECK(pool.ok);
        mesh::Packet *a = pool.allocNew(), *b = pool.allocNew();
        pool.queueOutbound(a, 0, 0); pool.queueInbound(b, 0);
        pool.drain(); LS_EQ_INT(pool.getFreeCount(), 4);
    }
    LS_EQ_INT(ls_shim_heap_outstanding(), before);
}

LS_CASE(stopped_outbound_requests_are_refused)
{
    s_task = nullptr;
    LS_EQ_INT(ls_mesh_send_text_on(0, "draft"), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_mesh_send_dm_id("0123456789ABCDEF", "draft"), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_mesh_advertise(), ESP_ERR_INVALID_STATE);
}

extern int mesh_fake_rx_error, mesh_fake_send_error;
extern bool mesh_fake_send_done;
extern void mesh_fake_reset(void);

static void reset_port()
{
    s_task = nullptr; s_stop = false; s_radio_hold = s_radio_held = false;
    mesh_objects_free();
    if (s_msglog_buf) { heap_caps_free(s_msglog_buf); s_msglog_buf = nullptr; }
    memset(s_delivery, 0, sizeof(s_delivery));
    memset(s_ack, 0, sizeof(s_ack)); memset(s_sent, 0, sizeof(s_sent));
    memset(s_msgs, 0, sizeof(s_msgs)); s_msg_count = s_msg_head = 0;
    memset(s_peers, 0, sizeof(s_peers)); s_peer_count = 0;
    memset(&s_radio_cfg, 0, sizeof(s_radio_cfg)); s_radio_stored = false;
    s_auto_tx = false; s_auto_listen = true;
    s_msglog.open = s_peerlog.open = s_sightlog.open = false;
    s_outbox.expires = 0; s_on_air = s_rx_fault = false;
    ls_shim_ptr_in_dram_result = true;
    ls_shim_task_reset(); ls_shim_task_execute_on_create(pdFALSE);
    mesh_fake_reset();
}
static void owner_here() { s_task = xTaskGetCurrentTaskHandle(); }
static void stop_port() {
    ls_shim_task_set_state(eSuspended);
    LS_EQ_INT(ls_mesh_stop(), ESP_OK);
}

LS_CASE(owner_commands_and_snapshots_are_serialized)
{
    reset_port(); owner_here();
    const auto owner = s_task;
    std::atomic<bool> finished(false);
    std::thread caller([&]() {
        for (int i = 0; i < 40; i++) {
            int result = mesh_call([&]() -> int {
                LS_CHECK(xTaskGetCurrentTaskHandle() == owner);
                return note_msg("queued on owner", true, 0, NULL);
            });
            LS_CHECK(result >= 0 && result < LS_MESH_MAX_MSGS);
            ls_mesh_msg_t msg;
            LS_EQ_INT(ls_mesh_messages(&msg, 1), 1);
            LS_EQ_STR(msg.text, "queued on owner");
        }
        finished = true;
    });
    while (!finished) { mesh_calls_service(); vTaskDelay(1); }
    caller.join(); s_task = nullptr;
}

LS_CASE(incoming_sequence_ignores_delivery_and_history_reload)
{
    reset_port();
    uint32_t seq = ls_mesh_incoming_seq();
    int a = note_msg("outgoing", true, 0, NULL);
    LS_EQ_UINT(ls_mesh_incoming_seq(), seq);
    note_msg("incoming", false, 0, NULL);
    LS_EQ_UINT(ls_mesh_incoming_seq(), seq + 1);
    mesh::Packet packet; delivery_bind(&packet, a);
    delivery_state(&packet, LS_MSG_SENT);
    msglog_open(); msglog_flush(); msglog_open();
    LS_EQ_UINT(ls_mesh_incoming_seq(), seq + 1);
    reset_port();
}

LS_CASE(listen_preference_preserves_manual_transmit)
{
    reset_port(); LsRadio radio; s_radio = &radio; owner_here();
    radio.setTxEnabled(true);
    LS_EQ_INT(ls_mesh_set_auto(false, false), ESP_OK);
    LS_CHECK(radio.txEnabled());
    LS_EQ_INT(ls_mesh_set_auto(false, true), ESP_OK);
    radio.setTxEnabled(false);
    LS_EQ_INT(ls_mesh_set_auto(true, true), ESP_OK);
    LS_CHECK(!radio.txEnabled());
    s_task = nullptr; s_radio = nullptr;
}

LS_CASE(saved_zero_power_restores_and_manual_start_is_disarmed)
{
    reset_port(); ls_mesh_radio_t cfg = {};
    cfg.freq_hz = 910525000; cfg.sf = 7; cfg.bw_hz = 62500; cfg.power_dbm = 0;
    LS_EQ_INT(ls_mesh_set_radio(&cfg), ESP_OK);
    LS_EQ_INT(ls_mesh_set_auto(true, true), ESP_OK);
    LS_EQ_INT(ls_mesh_start(), ESP_OK);
    LS_EQ_INT(ls_lora_cfg()->power_dbm, 0);
    LS_CHECK(!ls_mesh_tx_enabled());
    stop_port();
    LS_EQ_INT(ls_mesh_start(), ESP_OK);
    LS_EQ_INT(ls_lora_cfg()->power_dbm, 0);
    stop_port(); reset_port();
}

LS_CASE(restarts_and_partial_allocation_failures_release_the_graph)
{
    reset_port(); unsigned before = ls_shim_heap_outstanding();
    for (int i = 0; i < 12; i++) {
        LS_EQ_INT(ls_mesh_start(), ESP_OK); stop_port();
        LS_CHECK(!s_mesh && !s_mgr && !s_radio);
    }
    reset_port(); LS_EQ_UINT(ls_shim_heap_outstanding(), before);
    for (unsigned i = 1; i <= 9; i++) {
        ls_shim_heap_reset(); ls_shim_heap_fail_from(i);
        LS_EQ_INT(ls_mesh_start(), ESP_ERR_NO_MEM);
        LS_CHECK(!s_mesh && !s_mgr && !s_radio);
        LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
    }
    ls_shim_heap_reset(); reset_port();
}

LS_CASE(peer_restore_deduplicates_and_null_query_counts)
{
    reset_port(); peerlog_open(); ls_mesh_peer_rec_t rec = {};
    strcpy(rec.id, "0123456789ABCDEF");
    ls_rlog_append(&s_peerlog, &rec); ls_rlog_append(&s_peerlog, &rec);
    peerlog_open(); LS_EQ_INT(ls_mesh_peers(NULL, 0), 1);
    peerlog_open(); LS_EQ_INT(ls_mesh_peers(NULL, 0), 1);
    reset_port();
}

LS_CASE(receive_failure_is_reported_and_held_sends_are_refused)
{
    reset_port(); mesh_fake_rx_error = ESP_FAIL;
    LS_EQ_INT(ls_mesh_start(), ESP_FAIL); LS_CHECK(s_rx_fault); LS_CHECK(!s_mesh);
    mesh_fake_rx_error = 0; LS_EQ_INT(ls_mesh_start(), ESP_OK); owner_here();
    s_radio_hold = true; ls_mesh_set_tx(true);
    LS_EQ_INT(ls_mesh_send_text_on(0, "held draft"), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_mesh_advertise(), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_mesh_radio_default(), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_mesh_stop(), ESP_ERR_INVALID_STATE);
    LS_CHECK(!ls_mesh_queue_dm("0123456789ABCDEF", "held"));
    ls_mesh_stats_t stats; ls_mesh_get_stats(&stats);
    LS_CHECK(stats.held && !stats.receiving);
    s_radio_hold = false; s_radio_held = true;
    LS_EQ_INT(ls_mesh_stop(), ESP_ERR_INVALID_STATE);
    s_radio_held = false; s_on_air = true;
    LS_EQ_INT(ls_mesh_radio_default(), ESP_ERR_INVALID_STATE);
    s_on_air = false; LS_EQ_INT(mesh_receive(), ESP_OK);
    ls_mesh_get_stats(&stats); LS_CHECK(stats.receiving && !stats.rx_fault);
    stop_port(); reset_port();
}

LS_CASE(real_dispatcher_completion_and_failure_affect_only_their_packet)
{
    reset_port(); LS_EQ_INT(ls_mesh_start(), ESP_OK); owner_here(); ls_mesh_set_tx(true);
    LS_EQ_INT(ls_mesh_send_text_on(0, "first"), ESP_OK);
    LS_EQ_INT(ls_mesh_send_text_on(0, "second"), ESP_OK);
    int first = (s_msg_head + LS_MESH_MAX_MSGS - 2) % LS_MESH_MAX_MSGS;
    int second = (first + 1) % LS_MESH_MAX_MSGS;
    mesh_fake_send_done = false; ls_shim_time_advance(2000); s_mesh->loop();
    LS_EQ_INT(s_msgs[first].state, LS_MSG_SENDING);
    LS_EQ_INT(s_msgs[second].state, LS_MSG_QUEUED);
    mesh_fake_send_done = true; s_mesh->loop();
    LS_EQ_INT(s_msgs[first].state, LS_MSG_SENT);
    LS_EQ_INT(s_msgs[second].state, LS_MSG_QUEUED);
    mesh_fake_send_error = ESP_FAIL; ls_shim_time_advance(2000); s_mesh->loop();
    LS_EQ_INT(s_msgs[second].state, LS_MSG_FAILED);
    stop_port(); reset_port();
}

LS_CASE(old_history_migrates_in_ring_order_without_sender_or_text_loss)
{
    reset_port(); FILE *f = tmpfile(); LS_CHECK(f != NULL); if (!f) return;
    ls_rlog_header_t h = {LS_RLOG_MAGIC, LS_RLOG_VERSION, sizeof(mesh_legacy_msg_t), LS_MESH_MAX_MSGS, 2, 1};
    static mesh_legacy_msg_t rows[LS_MESH_MAX_MSGS]; memset(rows, 0, sizeof(rows));
    strcpy(rows[23].text, "older message"); rows[23].state = LS_MSG_IN;
    strcpy(rows[0].text, "newer message"); rows[0].state = LS_MSG_ACKED; rows[0].mine = true;
    fwrite(&h, sizeof(h), 1, f); fwrite(rows, sizeof(rows), 1, f); rewind(f);
    LS_EQ_INT(msglog_restore_legacy(f), 1);
    LS_EQ_INT(s_msg_count, 2); LS_EQ_STR(s_msgs[0].text, "older message");
    LS_EQ_STR(s_msgs[1].text, "newer message"); LS_EQ_INT(s_msgs[1].state, LS_MSG_ACKED);
    fclose(f); reset_port();
}

LS_CASE(failed_task_creation_unwinds_and_retry_can_start)
{
    reset_port(); ls_shim_task_fail_create(pdTRUE);
    LS_EQ_INT(ls_mesh_start(), ESP_ERR_NO_MEM);
    LS_CHECK(!s_mesh && !s_radio && !s_task);
    ls_shim_task_fail_create(pdFALSE);
    LS_EQ_INT(ls_mesh_start(), ESP_OK); stop_port(); reset_port();
}

LS_CASE(listener_pacing_tracks_modulation_and_bounds_bad_config)
{
    LS_EQ_UINT(ls_mesh_poll_ms(7, 62500, 32), 47);
    LS_EQ_UINT(ls_mesh_poll_ms(8, 62500, 32), 96);
    LS_EQ_UINT(ls_mesh_poll_ms(9, 62500, 16), 63);
    LS_EQ_UINT(ls_mesh_poll_ms(12, 62500, 16), 100);
    LS_EQ_UINT(ls_mesh_poll_ms(7, 500000, 32), 4);
    LS_EQ_UINT(ls_mesh_poll_ms(7, 0, 32), 1);
    LS_EQ_UINT(ls_mesh_poll_ms(255, 62500, 32), 1);
    LS_EQ_UINT(ls_mesh_poll_ms(7, 62500, 8), 1);
    for (unsigned sf = 5; sf <= 12; ++sf) {
        for (unsigned bw = 62500; bw <= 500000; bw *= 2) {
            unsigned pre = sf <= 8 ? 32 : 16;
            uint32_t ms = ls_mesh_poll_ms(sf, bw, pre);
            LS_CHECK(ms >= 1 && ms <= 100);
            if (ms > 1) LS_CHECK((uint64_t)(ms + 2) * bw <=
                                (uint64_t)(pre - 8) * (1u << sf) * 1000);
        }
    }
}

extern unsigned mesh_fake_polls;
extern int mesh_fake_packet;
LS_CASE(packet_pending_between_polls_is_collected_at_deadline)
{
    reset_port(); ls_shim_time_set(1000000);
    LS_EQ_INT(ls_mesh_start(), ESP_OK); owner_here();
    uint8_t byte = 0;
    LS_EQ_INT(s_radio->recvRaw(&byte, 1), 0);
    mesh_fake_packet = 1;
    ls_shim_time_advance(46999);
    LS_EQ_INT(s_radio->recvRaw(&byte, 1), 0);
    LS_EQ_INT(mesh_fake_packet, 1);
    ls_shim_time_advance(1);
    LS_EQ_INT(s_radio->recvRaw(&byte, 1), 1);
    LS_EQ_INT(byte, 0x5a); LS_EQ_UINT(s_radio->rx(), 1);
    ls_shim_time_advance(47000); mesh_fake_packet = -1;
    LS_EQ_INT(s_radio->recvRaw(&byte, 1), 0);
    LS_EQ_UINT(s_radio->bad(), 1);
    stop_port(); reset_port();
}

LS_CASE(idle_dispatcher_wakes_do_not_multiply_radio_bus_polls)
{
    reset_port(); ls_shim_time_set(1000000);
    LS_EQ_INT(ls_mesh_start(), ESP_OK);
    LS_EQ_INT(ls_shim_task_last_core_id(), 0);
    LS_EQ_INT(ls_shim_task_last_priority(), 4);
    owner_here(); mesh_fake_polls = 0;
    unsigned heap = ls_shim_heap_outstanding();
    /* Model frequent stats/command wakes for one second, using real Dispatcher. */
    for (int i = 0; i < 1000; ++i) {
        s_mesh->loop(); ls_shim_time_advance(1000);
    }
    LS_EQ_UINT(mesh_fake_polls, 22);
    LS_EQ_UINT(ls_shim_heap_outstanding(), heap);
    LS_CHECK(mesh_wait_ticks() > pdMS_TO_TICKS(10));
    s_on_air = true; LS_EQ_UINT(mesh_wait_ticks(), pdMS_TO_TICKS(10));
    s_on_air = false;
    mesh::Packet *p = s_mgr->allocNew();
    s_mgr->queueInbound(p, UINT32_MAX / 2);
    LS_EQ_UINT(mesh_wait_ticks(), pdMS_TO_TICKS(10));
    s_mgr->drain();
    /* Retuning from slow to fast must invalidate the previous RX deadline. */
    ls_lora_cfg_t cfg = *ls_lora_cfg(); cfg.bw_hz = 500000;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    s_mesh->loop(); LS_EQ_UINT(mesh_fake_polls, 23);
    LS_CHECK(mesh_wait_ticks() <= pdMS_TO_TICKS(4));
    stop_port(); reset_port();
}

LS_CASE(outbox_and_leases_notify_sleeping_mesh_owner)
{
    reset_port(); LS_EQ_INT(ls_mesh_start(), ESP_OK); owner_here();
    ls_mesh_set_tx(true);
    unsigned before = ls_shim_task_notify_count();
    LS_CHECK(ls_mesh_queue_dm("0123456789ABCDEF", "wake"));
    LS_EQ_UINT(ls_shim_task_notify_count(), before + 1);
    (void)ls_mesh_radio_hold(true);
    LS_EQ_UINT(ls_shim_task_notify_count(), before + 2);
    LS_CHECK(ls_mesh_radio_hold(false));
    LS_EQ_UINT(ls_shim_task_notify_count(), before + 3);
    stop_port(); reset_port();
}
