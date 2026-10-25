/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/ringbuf.h"
#include "ssh_vfs.h"

#define MAX_CLIENTS 4
static const char* TAG = "ssh_vfs";

extern FILE *backup_in;
extern FILE *backup_out;

static ssize_t ssh_vfs_write(void* ctx, int fd, const void * data, size_t size);
static ssize_t ssh_vfs_read(void* ctx, int fd, void * dst, size_t size);
static int ssh_vfs_open(void* ctx, const char * path, int flags, int mode);
static int ssh_vfs_close(void* ctx, int fd);
static int ssh_vfs_fstat(void* ctx, int fd, struct stat * st);

typedef struct {
    ssh_channel ssh_handle;
    bool opened;
    RingbufHandle_t ssh_rb;
} ssh_vfs_desc_t;

static ssh_vfs_desc_t s_desc[MAX_CLIENTS];
static _lock_t s_lock;
static ssh_vfs_config_t s_config;
/**
 * @brief Register the WebSocket client VFS with the given configuration.
 *
 * @param config Pointer to the configuration structure.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ssh_vfs_register(const ssh_vfs_config_t *config)
{
    s_config = *config;
    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = ssh_vfs_open,
        .close_p = ssh_vfs_close,
        .read_p = ssh_vfs_read,
        .write_p = ssh_vfs_write,
        .fstat_p = ssh_vfs_fstat,
    };
    return esp_vfs_register(config->base_path, &vfs, NULL);
}


/**
 * @brief Write data to the WebSocket client.
 *
 * @param ctx Context pointer (unused).
 * @param fd File descriptor.
 * @param data Pointer to data to write.
 * @param size Size of data to write.
 * @return Number of bytes written, or -1 on error.
 */
static ssize_t ssh_vfs_write(void* ctx, int fd, const void * data, size_t size)
{
    int sent = 0;
    const uint8_t *buf = (const uint8_t *)data;

    if (!buf || size == 0 || size > 32768) {
        return 0; // Nothing to write
    }

    // fprintf(backup_out, "Write string: %d\n", (int)size);
    // fflush(backup_out);


    if (fd < 0 || fd > MAX_CLIENTS) {
        errno = EBADF;
        return -1;
    }

    ssh_channel channel = s_desc[fd].ssh_handle;
    if (!channel || ssh_channel_is_eof(channel)) {
        errno = EPIPE;
        return -1;
    }

    if (!ssh_channel_is_eof(channel)) {
        sent = ssh_channel_write(channel, buf, size);
        for (int i = 0; i < size; i++) {
            fputc(((uint8_t*)buf)[i], backup_out);
        }
        fflush(backup_out);
        if (sent < 0) {
            errno = EIO;
            return -1;
        }
        if (buf[size-1] == '\n') {
            sent += ssh_channel_write(channel, "\r", strlen("\r"));
        }
    }

    return sent;
}


/**
 * @brief Directly write SSH channel data to the ring buffer.
 *
 * @param handle SSH channel handle.
 * @param data Pointer to data buffer.
 * @param size Size of data buffer.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ssh_vfs_push_data(ssh_channel handle, const void *data, int size)
{
    int fd;
    for (fd = 0; fd < MAX_CLIENTS; ++fd) {
        if (s_desc[fd].ssh_handle == handle) {
            break;
        }
    }
    if (fd == MAX_CLIENTS) {
        // didn't find the handle
        return ESP_ERR_INVALID_ARG;
    }
    RingbufHandle_t rb = s_desc[fd].ssh_rb;
    if (xRingbufferSend(rb, data, size, s_config.recv_timeout_ms) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}


/**
 * @brief Read data from the WebSocket client.
 *
 * @param ctx Context pointer (unused).
 * @param fd File descriptor.
 * @param dst Destination buffer.
 * @param size Number of bytes to read.
 * @return Number of bytes read, or -1 on error.
 */
static ssize_t ssh_vfs_read(void* ctx, int fd, void * dst, size_t size)
{
    size_t read_remaining = size;
    uint8_t* p_dst = (uint8_t*) dst;
    RingbufHandle_t rb = s_desc[fd].ssh_rb;

    if (fd < 0 || fd >= MAX_CLIENTS || s_desc[fd].ssh_rb == NULL) {
        errno = EBADF;
        return -1;
    }

    while (read_remaining > 0) {
        size_t read_size;
        void * ptr = xRingbufferReceiveUpTo(rb, &read_size, portMAX_DELAY, read_remaining);
        if (ptr == NULL) {
            // timeout
            errno = EIO;
            break;
        }
        memcpy(p_dst, ptr, read_size);


        vRingbufferReturnItem(rb, ptr);
        read_remaining -= read_size;
    }
    return size - read_remaining;
}


/**
 * @brief Open a WebSocket client VFS file descriptor.
 *
 * @param ctx Context pointer (unused).
 * @param path Path to open.
 * @param flags Open flags.
 * @param mode Open mode.
 * @return File descriptor on success, -1 on error.
 */
static int ssh_vfs_open(void* ctx, const char * path, int flags, int mode)
{
    if (path[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    int fd = strtol(path + 1, NULL, 10);
    if (fd < 0 || fd >= MAX_CLIENTS) {
        errno = ENOENT;
        return -1;
    }
    int res = -1;
    _lock_acquire(&s_lock);
    if (s_desc[fd].opened) {
        errno = EPERM;
    } else {
        s_desc[fd].opened = true;
        res = fd;
    }
    _lock_release(&s_lock);
    return res;
}

/**
 * @brief Close a WebSocket client VFS file descriptor.
 *
 * @param ctx Context pointer (unused).
 * @param fd File descriptor to close.
 * @return 0 on success, -1 on error.
 */
static int ssh_vfs_close(void* ctx, int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS) {
        errno = EBADF;
        return -1;
    }
    int res = -1;
    _lock_acquire(&s_lock);
    if (!s_desc[fd].opened) {
        errno = EBADF;
    } else {
        s_desc[fd].opened = false;
        res = 0;
    }
    _lock_release(&s_lock);
    return res;
}

/**
 * @brief Get file status for a WebSocket client VFS file descriptor.
 *
 * @param ctx Context pointer (unused).
 * @param fd File descriptor.
 * @param st Pointer to stat structure to fill.
 * @return 0 on success.
 */
static int ssh_vfs_fstat(void* ctx, int fd, struct stat * st)
{
    *st = (struct stat) {
        0
    };
    st->st_mode = S_IFCHR;
    return 0;
}

/**
 * @brief Add a WebSocket client to the VFS.
 *
 * @param handle WebSocket client handle.
 * @param id Client ID.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ssh_vfs_add_client(ssh_channel handle, int id)
{
    esp_err_t res = ESP_OK;
    _lock_acquire(&s_lock);
    if (s_desc[id].ssh_handle != NULL) {
        ESP_LOGE(TAG, "%s: id=%d already in use", __func__, id);
        res = ESP_ERR_INVALID_STATE;
    } else {
        ESP_LOGD(TAG, "%s: id=%d is now in use for ssh client handle=%p", __func__, id, handle);
        s_desc[id].ssh_handle = handle;
        s_desc[id].opened = false;
        s_desc[id].ssh_rb = xRingbufferCreate(s_config.recv_buffer_size, RINGBUF_TYPE_BYTEBUF);
    }
    _lock_release(&s_lock);
    return res;
}

/**
 * @brief Remove a WebSocket client from the VFS.
 *
 * @param handle WebSocket client handle.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ssh_vfs_del_client(ssh_channel handle)
{
    esp_err_t res = ESP_ERR_INVALID_ARG;
    _lock_acquire(&s_lock);
    for (int id = 0; id < MAX_CLIENTS; ++id) {
        if (s_desc[id].ssh_handle != handle) {
            continue;
        }
        if (s_desc[id].ssh_handle != NULL) {
            ESP_LOGE(TAG, "%s: id=%d already in use", __func__, id);
            res = ESP_ERR_INVALID_STATE;
            break;
        } else {
            ESP_LOGD(TAG, "%s: id=%d is now in use for ssh client handle=%p", __func__, id, handle);
            s_desc[id].ssh_handle = NULL;
            s_desc[id].opened = false;
            vRingbufferDelete(s_desc[id].ssh_rb);
            res = ESP_OK;
            break;
        }
    }
    _lock_release(&s_lock);
    return res;
}
