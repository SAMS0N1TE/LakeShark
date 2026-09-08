#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/*LS-761*/
/* Shell back-navigation gateway.
 *
 * exitToLauncher used to return true and do nothing, and LsShell::goBack
 * discarded the return value from LsApp::back().  Between them the back
 * contract and Files' visible root-up button both failed to reach HOME.
 * The decision code lives here so the bench can drive it against fake hooks
 * without dragging LVGL onto the host build, and so both back() and the root
 * up-button share exactly one "am I at the roots" predicate. */

typedef struct {
    /* Ask the shell to launch HOME.  A no-op hook is legal - the caller then
       receives false from the exit / dispatch functions below. */
    void (*go_home)(void *ctx);
    /* Whether the app with this registered name is the HOME app.  The default
       compares against the literal "HOME" that AppHome uses. */
    bool (*is_home_app)(const char *name);
    void *ctx;
} ls_shell_nav_hooks_t;

/* Install hooks.  Pass NULL to clear them (used by tests). */
void ls_shell_nav_configure(const ls_shell_nav_hooks_t *hooks);

/* An app's exitToLauncher() calls this.
 *
 * `caller_is_current` protects a backgrounded app from yanking the user off
 * the visible one.  `caller_name` lets the gateway skip the launch when HOME
 * itself is calling - that is the recursion the queue task calls out. */
bool ls_shell_nav_exit_to_launcher(bool caller_is_current, const char *caller_name);

/* LsShell::goBack calls this with the return value of the current app's
 * back().  When the app reports "unhandled" (false), the shell goes HOME so
 * the user is never stranded on an app whose back callback silently gave up. */
bool ls_shell_nav_dispatch_back(bool app_back_handled);

/* Files' Up-button / back() at the roots decision.  Returns true when the
 * cwd is the roots view, meaning the caller should ask the shell to go HOME
 * rather than pop a path component. */
bool ls_shell_nav_fb_up_is_home(const char *cwd);

#ifdef __cplusplus
}
#endif
