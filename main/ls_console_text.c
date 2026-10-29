/* Console help kept in text packs (components/ls_textpack).

   Command tables write their help and hint as LS_TXT(id, "..."), which in a
   packed build is a tag, not a pointer. Every esp_console_cmd_register call
   in the image comes through here (-Wl,--wrap, main/CMakeLists.txt): the
   descriptor is copied with its help and hint turned back into text, and the
   console stores that. esp_console keeps the help pointer (decoded frames
   live for the whole boot) and copies the hint, so nothing else changes:
   `help` prints exactly what the literal said. */

#include "esp_console.h"
#include "ls_textpack.h"

#if defined(LS_TEXT_PACKED) && LS_TEXT_PACKED

esp_err_t __real_esp_console_cmd_register(const esp_console_cmd_t *cmd);

esp_err_t __wrap_esp_console_cmd_register(const esp_console_cmd_t *cmd)
{
    if (!cmd) return __real_esp_console_cmd_register(cmd);
    esp_console_cmd_t c = *cmd;
    c.help = ls_text_str(c.help);
    c.hint = ls_text_str(c.hint);
    return __real_esp_console_cmd_register(&c);
}

#endif
