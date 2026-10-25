/*
 * Simple SSH Server Example
 *
 * This is a minimal SSH server that:
 * - Listens on 0.0.0.0 (all interfaces)
 * - Accepts password authentication
 * - Provides a simple shell
 * - Uses the built libssh library
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "protocol_examples_common.h"
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

// Configuration (from Kconfig)
#include "sdkconfig.h"

#define DEFAULT_PORT CONFIG_EXAMPLE_DEFAULT_PORT
#define DEBUG_LEVEL CONFIG_EXAMPLE_DEBUG_LEVEL
#define ALLOW_PASSWORD_AUTH (CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH)
#define ALLOW_PUBLICKEY_AUTH (CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH)
#define DEFAULT_USERNAME CONFIG_EXAMPLE_DEFAULT_USERNAME

// Authentication methods
#if ALLOW_PASSWORD_AUTH
#define DEFAULT_PASSWORD CONFIG_EXAMPLE_DEFAULT_PASSWORD
#endif
#if ALLOW_PASSWORD_AUTH && ALLOW_PUBLICKEY_AUTH
#define ALLOW_AUTH_METHODS (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_PUBLICKEY)
#elif ALLOW_PASSWORD_AUTH
#define ALLOW_AUTH_METHODS (SSH_AUTH_METHOD_PASSWORD)
#elif ALLOW_PUBLICKEY_AUTH
#define ALLOW_AUTH_METHODS (SSH_AUTH_METHOD_PUBLICKEY)
#else
#define ALLOW_AUTH_METHODS (0)
#endif

static int authenticated = 0;
static int tries = 0;
static ssh_channel channel = NULL;

// Callbacks
static int shell_request(ssh_session session, ssh_channel channel, void *userdata)
{
    printf("[DEBUG] Shell requested\n");

    if (channel == NULL) {
        printf("[DEBUG] Shell requested but channel is NULL\n");
        return SSH_ERROR;
    }

    printf("[DEBUG] Shell setup completed successfully\n");
    return SSH_OK;
}

static int pty_request(ssh_session session, ssh_channel channel,
                      const char *term, int cols, int rows,
                      int py, int px, void *userdata)
{
    printf("[DEBUG] PTY requested: %s (%dx%d)\n", term, cols, rows);
    return SSH_OK;
}


// Authentication callback - deny none authentication
static int auth_none(ssh_session session, const char *user, void *userdata)
{
    printf("[DEBUG] Auth none requested for user: %s\n", user);
    ssh_set_auth_methods(session, ALLOW_AUTH_METHODS);
    return SSH_AUTH_DENIED;
}

#if ALLOW_PASSWORD_AUTH
// Password authentication callback
static int auth_password(ssh_session session, const char *user,
                        const char *password, void *userdata)
{

    printf("[DEBUG] Password auth attempt for user: %s\n", user);

    if (strcmp(user, DEFAULT_USERNAME) == 0 &&
        strcmp(password, DEFAULT_PASSWORD) == 0) {
        authenticated = 1;
        printf("[DEBUG] Authentication successful for user: %s\n", user);
        return SSH_AUTH_SUCCESS;
    }

    tries++;
    if (tries >= 3) {
        printf("[DEBUG] Too many authentication attempts\n");
        ssh_disconnect(session);
        return SSH_AUTH_DENIED;
    }

    printf("[DEBUG] Authentication failed (attempt %d/3)\n", tries);
    return SSH_AUTH_DENIED;
}
#endif // ALLOW_PASSWORD_AUTH

#if ALLOW_PUBLICKEY_AUTH
/* Public key authentication using in-memory authorized_keys list */
static int auth_publickey(ssh_session session,
                         const char *user,
                         struct ssh_key_struct *pubkey,
                         char signature_state,
                         void *userdata)
{
    extern const uint8_t allowed_pubkeys[]   asm("_binary_ssh_allowed_client_key_pub_start");

    if (user == NULL || strcmp(user, DEFAULT_USERNAME) != 0) {
        return SSH_AUTH_DENIED;
    }
    ESP_LOGI("DEBUG", "Public key authentication requested for user: %s", user);

    /* If client is probing supported keys (no signature), accept match to prompt signature */
    const char *cursor = (const char *)allowed_pubkeys;
    while (cursor != NULL && *cursor != '\0') {
        const char *line_start = cursor;
        const char *nl = strchr(cursor, '\n');
        size_t line_len = (nl != NULL) ? (size_t)(nl - line_start) : strlen(line_start);

        /* Advance cursor for next iteration now to simplify continues */
        cursor = (nl != NULL) ? nl + 1 : line_start + line_len;

        /* Skip empty/whitespace-only lines */
        size_t leading_ws = 0U;
        while (leading_ws < line_len && isspace((unsigned char)line_start[leading_ws])) {
            leading_ws++;
        }
        if (leading_ws >= line_len) {
            continue;
        }

        /* Make a NUL-terminated copy of the current line */
        char *line = (char *)malloc(line_len + 1);
        if (line == NULL) {
            ESP_LOGI("DEBUG", "malloc failed at %d", __LINE__);
            break;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        /* Find end of type token (first whitespace) */
        const char *sp1 = line;
        while (*sp1 != '\0' && !isspace((unsigned char)*sp1)) {
            sp1++;
        }
        if (*sp1 == '\0') {
            free(line);
            continue;
        }
        size_t type_len = (size_t)(sp1 - line);
        if (type_len == 0) {
            free(line);
            continue;
        }

        char type_name[32];
        if (type_len >= sizeof(type_name)) {
            free(line);
            continue;
        }
        memcpy(type_name, line, type_len);
        type_name[type_len] = '\0';

        /* Skip whitespace to start of base64 */
        const char *b64_start = sp1;
        while (*b64_start != '\0' && isspace((unsigned char)*b64_start)) {
            b64_start++;
        }
        if (*b64_start == '\0' || *b64_start == '\n' || *b64_start == '\r') {
            free(line);
            continue;
        }
        /* Find end of base64 (next whitespace or end) */
        const char *p = b64_start;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t b64_len = (size_t)(p - b64_start);
        if (b64_len == 0) {
            free(line);
            continue;
        }

        enum ssh_keytypes_e key_type = ssh_key_type_from_name(type_name);
        if (key_type == SSH_KEYTYPE_UNKNOWN) {
            free(line);
            continue;
        }

        /* Copy only the base64 blob (exclude trailing comment) */
        char *b64_copy = (char *)malloc(b64_len + 1);
        if (b64_copy == NULL) {
            ESP_LOGI("DEBUG", "malloc failed at %d", __LINE__);
            free(line);
            continue;
        }
        memcpy(b64_copy, b64_start, b64_len);
        b64_copy[b64_len] = '\0';

        ssh_key authorized_key = NULL;
        int rc = ssh_pki_import_pubkey_base64(b64_copy, key_type, &authorized_key);
        free(b64_copy);
        if (rc != SSH_OK || authorized_key == NULL) {
            if (authorized_key != NULL) {
                ssh_key_free(authorized_key);
            }
            free(line);
            continue;
        }
        rc = ssh_key_cmp(authorized_key, pubkey, SSH_KEY_CMP_PUBLIC);
        ssh_key_free(authorized_key);
        if (rc == 0) {
            free(line);
            if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
                return SSH_AUTH_SUCCESS; /* tell client to sign */
            }

            if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
                authenticated = 1;
                ESP_LOGI("DEBUG", "Public key authentication successful for user: %s", user);
                return SSH_AUTH_SUCCESS;
            }

            return SSH_AUTH_DENIED;
        }

        free(line);
    }

    return SSH_AUTH_DENIED;
}
#endif // ALLOW_PUBLICKEY_AUTH

struct ssh_channel_callbacks_struct channel_cb = {
    .userdata = NULL,
    .channel_pty_request_function = pty_request,
    .channel_shell_request_function = shell_request,
};

static ssh_channel channel_open(ssh_session session, void *userdata) {
    (void)userdata;

    if (channel != NULL) {
        printf("[DEBUG] Channel already exists\n");
        return NULL;
    }

    printf("[DEBUG] Opening new channel\n");
    channel = ssh_channel_new(session);

    ssh_callbacks_init(&channel_cb);
    ssh_set_channel_callbacks(channel, &channel_cb);

    printf("[DEBUG] Channel created and callbacks set\n");
    return channel;
}

static int set_hostkey(ssh_bind sshbind)
{
    extern const uint8_t hostkey[]   asm("_binary_ssh_host_ed25519_key_start");
    int rc = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, hostkey);
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to set hardcoded private key: %s\n", ssh_get_error(sshbind));
        return SSH_ERROR;
    }

    printf("[DEBUG] Successfully loaded hardcoded private key\n");
    return SSH_OK;
}

void app_main(void)
{
    ssh_bind sshbind;
    ssh_session session;
    ssh_event event;
    int rc;
    const char *port = DEFAULT_PORT;

    // Initialize ESP-IDF components
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // Initialize libssh
    rc = ssh_init();
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to initialize libssh: %d\n", rc);
        return;
    }

    // Create SSH bind object
    sshbind = ssh_bind_new();
    if (sshbind == NULL) {
        fprintf(stderr, "Failed to create SSH bind object\n");
        return;
    }

    // Set bind options
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT_STR, port);
#ifdef DEBUG_LEVEL
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, DEBUG_LEVEL);
#endif

    // Set host key
    rc = set_hostkey(sshbind);
    if (rc != SSH_OK) {
        ssh_bind_free(sshbind);
        return;
    }

    // Listen for connections
    rc = ssh_bind_listen(sshbind);
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to listen on 0.0.0.0:%s: %s\n",
                port, ssh_get_error(sshbind));
        ssh_bind_free(sshbind);
        return;
    }

    printf("Simple SSH Server listening on 0.0.0.0:%s\n", port);
#if ALLOW_PASSWORD_AUTH
    printf("Default credentials: %s/%s\n", DEFAULT_USERNAME, DEFAULT_PASSWORD);
#endif

    // Accept connections
    while (1) {
        session = ssh_new();
        if (session == NULL) {
            fprintf(stderr, "Failed to create session\n");
            continue;
        }

        rc = ssh_bind_accept(sshbind, session);
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to accept connection: %s\n",
                    ssh_get_error(sshbind));
            ssh_free(session);
            continue;
        }

        printf("[DEBUG] New connection accepted\n");

        // Set up server callbacks
        struct ssh_server_callbacks_struct server_cb = {
            .userdata = NULL,
            .auth_none_function = auth_none,
#if ALLOW_PASSWORD_AUTH
            .auth_password_function = auth_password,
#endif
#if ALLOW_PUBLICKEY_AUTH
            .auth_pubkey_function = auth_publickey,
#endif
            .channel_open_request_session_function = channel_open
        };

        ssh_callbacks_init(&server_cb);
        ssh_set_server_callbacks(session, &server_cb);
        printf("[DEBUG] Server callbacks set\n");

        // Handle key exchange
        // Note: ssh_handle_key_exchange() can return:
        // - SSH_OK: Key exchange completed successfully
        // - SSH_AGAIN: Key exchange in progress, need to call again
        // - SSH_ERROR: Fatal error occurred
        rc = ssh_handle_key_exchange(session);
        printf("[DEBUG] Key exchange result: rc=%d ", rc);
        if (rc == SSH_OK) {
            printf("(SSH_OK - completed successfully)\n");
        } else if (rc == SSH_AGAIN) {
            printf("(SSH_AGAIN - in progress)\n");
        } else if (rc == SSH_ERROR) {
            printf("(SSH_ERROR - fatal error)\n");
        } else {
            printf("(unknown return code %d)\n", rc);
        }

        if (rc == SSH_ERROR) {
            fprintf(stderr, "[DEBUG] Key exchange failed: %s (bind: %s)\n",
                    ssh_get_error(session), ssh_get_error(sshbind));
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        printf("[DEBUG] Key exchange completed or in progress\n");

        // Set up authentication methods
        ssh_set_auth_methods(session, ALLOW_AUTH_METHODS);
        printf("[DEBUG] Authentication methods set\n");

        // Create event for session handling
        event = ssh_event_new();
        if (event == NULL) {
            fprintf(stderr, "[DEBUG] Failed to create event\n");
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        // Add session to event
        if (ssh_event_add_session(event, session) != SSH_OK) {
            fprintf(stderr, "[DEBUG] Failed to add session to event\n");
            ssh_event_free(event);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }

        printf("[DEBUG] Session added to event, starting main loop\n");

        // Check initial channel state
        printf("[DEBUG] Initial channel state: channel=%p, is_open=%s\n",
               channel, channel ? (ssh_channel_is_open(channel) ? "yes" : "no") : "NULL");

        // Wait for authentication and channel creation (like the official example)
        int n = 0;
        while (authenticated == 0 || channel == NULL) {
            printf("[DEBUG] Waiting for auth/channel: auth=%s, channel=%p (attempt %d)\n",
                   authenticated ? "yes" : "no", channel, n);

            // If the user has used up all attempts, or if he hasn't been able to
            // authenticate in 10 seconds (n * 100ms), disconnect.
            if (tries >= 3 || n >= 100) {
                printf("[DEBUG] Timeout waiting for authentication/channel\n");
                break;
            }

            if (ssh_event_dopoll(event, 10000) == SSH_ERROR) {
                printf("[DEBUG] Error polling events: %s\n", ssh_get_error(session));
                break;
            }
            n++;
        }

        // If we have a channel, set up callbacks and continue
        if (channel != NULL) {
            printf("[DEBUG] Channel created, setting up callbacks\n");
            #define BUF_SIZE 2048
            static char buf[BUF_SIZE];
            int i;
            int count = 0;
            char command[100];

            printf("[DEBUG] Channel created, setting up callbacks\n");

            do {
                i = ssh_channel_read(channel, buf, sizeof(buf) - 1, 0);
                if (i > 0) {
                    if (ssh_channel_write(channel, buf, i) == SSH_ERROR) {
                        printf("error writing to channel\n");
                        return;
                    }

                    buf[i] = '\0';
                    printf("%s", buf);
                    fflush(stdout);
                    if (count < 100) {
                        memcpy(command + count, buf, i);
                        count += i;
                    }

                    if (buf[0] == '\x0d') {
                        printf("[INFO] Command: %s", command);
                        if (memcmp(command, "exit", 4) == 0) {
                            printf("[DEBUG] Exit command received\n");
                            break;
                        }
                        if (memcmp(command, "reset", 5) == 0) {
                            printf("[DEBUG] Reset command received\n");
                            esp_restart();
                            // break;
                        }
                        if (memcmp(command, "hello", 5) == 0) {
                            printf("[DEBUG] Hello command received\n");
                            if (ssh_channel_write(channel, "Hello, world!\n", 14) == SSH_ERROR) {
                                printf("error writing to channel\n");
                                return;
                            }
                        }


                        count = 0;
                        if (ssh_channel_write(channel, "\n", 1) == SSH_ERROR) {
                            printf("error writing to channel\n");
                            return;
                        }

                        printf("\n");
                    }
                }
            } while (i>0);

        }

        printf("[DEBUG] Connection closed\n");
        if (channel != NULL) {
            ssh_channel_free(channel);
            channel = NULL;
        }
        authenticated = 0;
        tries = 0;
        ssh_event_free(event);
        ssh_disconnect(session);
        ssh_free(session);
    }

    // Clean up
    ssh_bind_free(sshbind);
    ssh_finalize();
}
