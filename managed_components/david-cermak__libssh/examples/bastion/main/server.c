// modular_ssh_server.c
// A modular version of the simple SSH server for ESP32 using libssh

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "protocol_examples_common.h"
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>
#include "console_simple_init.h"
#include "ssh_vfs.h"

static const char* TAG = "ssh_server";

#define DEFAULT_PORT "22"
#define DEFAULT_USERNAME "user"
#define DEFAULT_PASSWORD "password"

static int authenticated = 0;
static int tries = 0;
static ssh_channel channel = NULL;

FILE *backup_out;

// ---- Function Prototypes ----
static void handle_shell_io(ssh_channel channel);

static int set_hostkey(ssh_bind sshbind) {
    extern const uint8_t hostkey[] asm("_binary_ssh_host_ed25519_key_start");
    int rc = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, hostkey);
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to set hardcoded private key: %s\n", ssh_get_error(sshbind));
        return SSH_ERROR;
    }
    return SSH_OK;
}

// ---- Authentication Callbacks ----
static int auth_none(ssh_session session, const char *user, void *userdata) {
    ESP_LOGI(TAG, "[DEBUG] Auth none requested for user: %s\n", user);
    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);
    return SSH_AUTH_DENIED;
}

static int auth_password(ssh_session session, const char *user, const char *password, void *userdata) {
    ESP_LOGI(TAG, "[DEBUG] Password auth attempt for user: %s\n", user);
    if (strcmp(user, DEFAULT_USERNAME) == 0 && strcmp(password, DEFAULT_PASSWORD) == 0) {
        authenticated = 1;
        return SSH_AUTH_SUCCESS;
    }
    tries++;
    if (tries >= 3) {
        ssh_disconnect(session);
        return SSH_AUTH_DENIED;
    }
    return SSH_AUTH_DENIED;
}

// ---- Channel Request Callbacks ----
static int pty_request(ssh_session session, ssh_channel channel, const char *term, int cols, int rows, int py, int px, void *userdata) {
    ESP_LOGI(TAG, "[DEBUG] PTY requested: %s (%dx%d)\n", term, cols, rows);
    return SSH_OK;
}

static int shell_request(ssh_session session, ssh_channel channel, void *userdata) {
    ESP_LOGI(TAG, "[DEBUG] Shell requested\n");
    return SSH_OK;
}

static struct ssh_channel_callbacks_struct channel_cb = {
    .userdata = NULL,
    .channel_pty_request_function = pty_request,
    .channel_shell_request_function = shell_request,
};

// ---- Channel Open Callback ----
static ssh_channel channel_open(ssh_session session, void *userdata) {
    if (channel != NULL) return NULL;
    channel = ssh_channel_new(session);
    ssh_callbacks_init(&channel_cb);
    ssh_set_channel_callbacks(channel, &channel_cb);
    return channel;
}

// ---- Main Session Handler ----
static void handle_connection(ssh_session session) {
    ssh_event event = ssh_event_new();
    if (!event || ssh_event_add_session(event, session) != SSH_OK) return;

    int n = 0;
    while (authenticated == 0 || channel == NULL) {
        if (tries >= 3 || n >= 100) break;
        if (ssh_event_dopoll(event, 10000) == SSH_ERROR) break;
        n++;
    }

    if (channel) {
        handle_shell_io(channel);
    }

    if (channel) { ssh_channel_free(channel); channel = NULL; }
    ssh_event_free(event);
    ssh_disconnect(session);
    ssh_free(session);
    authenticated = 0;
    tries = 0;
}


/**
 * @brief Initialize VFS for WebSocket I/O redirection.
 * @return FILE pointer for WebSocket I/O, or NULL on failure.
 */
static FILE* vfs_init(void)
{
    // Configure the WebSocket VFS driver
    ssh_vfs_config_t config = {
        .base_path = "/ssh",
        .send_timeout_ms = 10000,
        .recv_timeout_ms = 10000,
        .recv_buffer_size = 2048,
        .fallback_stdout = stdout
    };
    ESP_ERROR_CHECK(ssh_vfs_register(&config));

    // Register the client with the VFS driver (index 0)
    ssh_vfs_add_client(channel, 0);

    FILE *ssh_io = fopen("/ssh/0", "r+");
    if (!ssh_io) {
        ESP_LOGE(TAG, "Failed to open ssh I/O file");
        return NULL;
    }

    backup_out = _GLOBAL_REENT->_stdout;

    _GLOBAL_REENT->_stdin = ssh_io;
    _GLOBAL_REENT->_stdout = ssh_io;
    _GLOBAL_REENT->_stderr = ssh_io;

    fprintf(backup_out, "Test string written to backup_out\n");
    fflush(backup_out);

    return ssh_io;
}


/**
 * @brief Clean up VFS resources.
 * @param ssh_io FILE pointer for WebSocket I/O.
 */
static void vfs_exit(FILE* ssh_io)
{
    if (ssh_io) {
        fclose(ssh_io);
        ssh_io = NULL;
    }
}


void tunnel_add_and_start(int p1, const char *host, int p2);

int do_tun(int argc, char **argv)
{
    printf("Creating tunnel...\n");
    if (argc < 4) {
        printf("Usage: tun <P1> <HOST> <P2>\n");
        return -1;
    }
    int p1 = atoi(argv[1]);
    const char *host = argv[2];
    int p2 = atoi(argv[3]);
    tunnel_add_and_start(p1, host, p2);
    return 0;
}

void tunnel_stop(int p1);

int do_tunkill(int argc, char **argv)
{
    printf("Stopping tunnel...\n");
    if (argc < 2) {
        printf("Usage: tunkill <P1>\n");
        return -1;
    }
    int p1 = atoi(argv[1]);
    tunnel_stop(p1);
    return 0;
}

// Runs the console REPL task for command processing.
static void console_task(void* arg)
{
#if 1
    // Initialize console REPL
    ESP_ERROR_CHECK(console_cmd_init());

    ESP_ERROR_CHECK(console_cmd_user_register("tun", do_tun));
    ESP_ERROR_CHECK(console_cmd_user_register("tunkill", do_tunkill));

    ESP_ERROR_CHECK(console_cmd_all_register());

    // start console REPL
    ESP_ERROR_CHECK(console_cmd_start());
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    vTaskDelete(NULL);
}


// Starts the console task with a delay for initialization.
static void run_console_task(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    xTaskCreate(console_task, "console_task", 16 * 1024, NULL, 5, NULL);
}


// Reads data from the SSH channel and pushes it to the VFS ringbuffer.
static void vfs_read_task(void* arg)
{
    #define BUF_SIZE 2048
    char buf[BUF_SIZE];
    int i = 0;
    ssh_channel channel_l = (ssh_channel)arg;
    esp_err_t res = ESP_OK;

    do {
        i = ssh_channel_read(channel_l, buf, sizeof(buf) - 1, 0);
        if (i > 0) {
            if (buf[0] != '\x0d') {
                // Write to the vfs ringbuffer
                res = ssh_vfs_push_data(channel_l, buf, i);
                if (res != ESP_OK) {
                    fprintf(stderr, "Failed to push data to VFS: %s\n", esp_err_to_name(res));
                    break;
                }
            } else {
                // Write to the vfs ringbuffer
                esp_err_t res = ssh_vfs_push_data(channel_l, "\n", strlen("\n"));
                if (res != ESP_OK) {
                    fprintf(stderr, "Failed to push data to VFS: %s\n", esp_err_to_name(res));
                    break;
                }
            }
        }
    } while (i > 0);

    vTaskDelete(NULL);
}


// Starts the VFS read task to handle channel input.
static void run_vfs_read_task(ssh_channel channel_l)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    xTaskCreate(vfs_read_task, "vfs_read_task", 16 * 1024, channel_l, 5, NULL);
}


// Manages shell I/O by initializing VFS, starting console and read tasks.
// ---- Shell I/O Handler ----
static void handle_shell_io(ssh_channel channel_l) {
    FILE* ssh_io = vfs_init();
    if (ssh_io == NULL) {
        ESP_LOGE(TAG, "Failed to open ssh I/O file");
        return;
    }

    run_console_task();

    run_vfs_read_task(channel_l);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vfs_exit(ssh_io);
}


void init_ssh_server(void)
{
    if (ssh_init() != SSH_OK) return;

    ssh_bind sshbind = ssh_bind_new();
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT_STR, DEFAULT_PORT);
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, "1");

    if (set_hostkey(sshbind) != SSH_OK || ssh_bind_listen(sshbind) != SSH_OK) {
        ssh_bind_free(sshbind);
        return;
    }

    ESP_LOGI(TAG, "Simple SSH Server listening on 0.0.0.0:%s\n", DEFAULT_PORT);
    ESP_LOGI(TAG, "Default credentials: %s/%s\n", DEFAULT_USERNAME, DEFAULT_PASSWORD);

    while (1) {
        ssh_session session = ssh_new();
        if (!session || ssh_bind_accept(sshbind, session) != SSH_OK) {
            if (session) ssh_free(session);
            continue;
        }

        struct ssh_server_callbacks_struct server_cb = {
            .userdata = NULL,
            .auth_none_function = auth_none,
            .auth_password_function = auth_password,
            .channel_open_request_session_function = channel_open
        };
        ssh_callbacks_init(&server_cb);
        ssh_set_server_callbacks(session, &server_cb);

        if (ssh_handle_key_exchange(session) == SSH_OK) {
            ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);
            handle_connection(session);
        } else {
            ssh_disconnect(session);
            ssh_free(session);
        }
    }

    ssh_bind_free(sshbind);
    ssh_finalize();
}
