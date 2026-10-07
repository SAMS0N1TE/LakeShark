#pragma once

/* Ending a task that was made with xTaskCreate*WithCaps (its stack in PSRAM).

   A task that deletes itself with vTaskDeleteWithCaps(NULL) has IDF create a
   helper task, with its stack and TCB in internal RAM, to free it, and IDF
   aborts when that allocation fails. On this board all internal RAM is
   DMA-capable and runs short while USB, the SDRs and the radios are busy,
   which is when these tasks end. The reaper is one task made at boot that
   deletes the others: deleting another task allocates nothing. */

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the reaper, once, early in a normal boot. Until it runs a retiring
   task deletes itself as before. */
void ls_task_reap_start(void);

/* Ends the calling task, which must have been made with
   xTaskCreate*WithCaps: hands it to the reaper and suspends. Never returns. */
void ls_task_retire_self(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
