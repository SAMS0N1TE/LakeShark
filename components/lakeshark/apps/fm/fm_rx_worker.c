#include "fm_rx_worker.h"

bool fm_rx_worker_start(TaskFunction_t worker)
{
    return ls_radio_decode_worker_start(worker, "fm_rx", 6, 1);
}

bool fm_rx_worker_release(unsigned wait_iterations, uint32_t wait_ms)
{
    return ls_radio_decode_worker_release(wait_iterations, wait_ms);
}
