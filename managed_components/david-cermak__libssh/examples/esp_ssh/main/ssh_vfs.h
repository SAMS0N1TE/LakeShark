/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#pragma once

#include "esp_err.h"
#include <libssh/libssh.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* base_path;
    int send_timeout_ms;
    int recv_timeout_ms;
    size_t recv_buffer_size;
    FILE* fallback_stdout;
} ssh_vfs_config_t;

esp_err_t ssh_vfs_register(const ssh_vfs_config_t *config);

esp_err_t ssh_vfs_add_client(ssh_channel handle, int id);

esp_err_t ssh_vfs_del_client(ssh_channel handle);

//esp_err_t ssh_vfs_event_handler(ssh_channel handle, int32_t event_id, const esp_websocket_event_data_t *event_data);

esp_err_t ssh_vfs_push_data(ssh_channel handle, const void *data, int size);

#ifdef __cplusplus
}
#endif
