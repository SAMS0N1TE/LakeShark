/*LS-761*/
/* See ls_shell_nav.h for the why.  The bench drives these three functions
   directly with counting hooks; the shell wires the real LsShell::home into
   `go_home` at begin() time. */

#include "shell/ls_shell_nav.h"

#include <string.h>

static ls_shell_nav_hooks_t s_hooks;

void ls_shell_nav_configure(const ls_shell_nav_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

static bool caller_is_home(const char *name)
{
    if (s_hooks.is_home_app) return s_hooks.is_home_app(name);
    return name && strcmp(name, "HOME") == 0;
}

static bool request_home(void)
{
    if (!s_hooks.go_home) return false;
    s_hooks.go_home(s_hooks.ctx);
    return true;
}

bool ls_shell_nav_exit_to_launcher(bool caller_is_current, const char *caller_name)
{
    if (!caller_is_current)          return false;
    if (caller_is_home(caller_name)) return false;
    return request_home();
}

bool ls_shell_nav_dispatch_back(bool app_back_handled)
{
    if (app_back_handled) return false;
    return request_home();
}

bool ls_shell_nav_fb_up_is_home(const char *cwd)
{
    return cwd == NULL || cwd[0] == '\0';
}
