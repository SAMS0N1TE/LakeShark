#ifndef LS_SHIM_ESP_ERR_H
#define LS_SHIM_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
/* used by ls_action_builtin, which lssim links for real. */
#define ESP_ERR_NOT_ALLOWED     0x106
/* The IDF's own numbering, continued: a board that declares no bus returns
   NOT_SUPPORTED and a part that will not answer returns TIMEOUT, and both
   are load-bearing distinctions in the radio drivers. */
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERROR_CHECK(x) ((void)(x))
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "esp_err"; }
#endif
