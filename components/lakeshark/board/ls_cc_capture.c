#include "ls_cc_capture.h"
#include "ls_board.h"
#ifdef LS_BOARD_MIX_CC_GDO0
#include "driver/rmt_rx.h"
#include "esp_heap_caps.h"
#include "rec_state.h"
#include "rec_watch.h"
#include <stdatomic.h>

#define SYMBOLS 512
static rmt_channel_handle_t channel;
static rmt_symbol_word_t *symbols;
static int32_t *pulses;
static atomic_uint completed;
static bool enabled;

static bool done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *event,
                 void *ctx) {
  (void)ch;
  (void)ctx;
  atomic_store_explicit(&completed, event->num_symbols + 1,
                        memory_order_release);
  return false;
}
static bool arm(void) {
  rmt_receive_config_t cfg = {.signal_range_min_ns = 1000,
                              .signal_range_max_ns = 30000000};
  atomic_store(&completed, 0);
  return rmt_receive(channel, symbols, SYMBOLS * sizeof(*symbols), &cfg) ==
         ESP_OK;
}
void ls_cc_capture_stop(void) {
  if (enabled)
    rmt_disable(channel);
  enabled = false;
  if (channel)
    rmt_del_channel(channel);
  channel = NULL;
  heap_caps_free(symbols);
  symbols = NULL;
  heap_caps_free(pulses);
  pulses = NULL;
  atomic_store(&completed, 0);
}
bool ls_cc_capture_start(void) {
  ls_cc_capture_stop();
  symbols = heap_caps_malloc(SYMBOLS * sizeof(*symbols),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  pulses = heap_caps_malloc(SYMBOLS * 2 * sizeof(*pulses),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  rmt_rx_channel_config_t cfg = {.gpio_num = LS_BOARD_MIX_CC_GDO0,
                                 .clk_src = RMT_CLK_SRC_DEFAULT,
                                 .resolution_hz = 1000000,
                                 .mem_block_symbols = 64};
  if (!symbols || !pulses || rmt_new_rx_channel(&cfg, &channel) != ESP_OK)
    goto fail;
  rmt_rx_event_callbacks_t callbacks = {.on_recv_done = done};
  if (rmt_rx_register_event_callbacks(channel, &callbacks, NULL) != ESP_OK ||
      rmt_enable(channel) != ESP_OK)
    goto fail;
  enabled = true;
  if (!arm())
    goto fail;
  return true;
fail:
  ls_cc_capture_stop();
  return false;
}
bool ls_cc_capture_poll(uint32_t hz, uint32_t *captures, uint32_t *overflows) {
  unsigned count = atomic_load_explicit(&completed, memory_order_acquire);
  if (!count)
    return true;
  count--;
  if (count >= SYMBOLS)
    (*overflows)++;
  else {
    int n = 0;
    for (unsigned i = 0; i < count; i++) {
      unsigned duration[] = {symbols[i].duration0, symbols[i].duration1};
      bool level[] = {symbols[i].level0, symbols[i].level1};
      for (int j = 0; j < 2; j++) {
        if (!duration[j] || (!n && !level[j]))
          continue;
        int32_t value = level[j] ? (int32_t)duration[j] : -(int32_t)duration[j];
        if (n && (pulses[n - 1] > 0) == level[j])
          pulses[n - 1] += value;
        else
          pulses[n++] = value;
      }
    }
    if (n >= 6) {
      rec_watch_submit_from(REC_SOURCE_CC1101, hz, pulses, n, 0, REC_END_GAP);
      (*captures)++;
    }
  }
  return arm();
}
#else
bool ls_cc_capture_start(void) { return false; }
void ls_cc_capture_stop(void) {}
bool ls_cc_capture_poll(uint32_t hz, uint32_t *captures, uint32_t *overflows) {
  (void)hz;
  (void)captures;
  (void)overflows;
  return false;
}
#endif
