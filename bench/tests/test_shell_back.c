/* LS_TEST_SOURCES: ${FW}/components/apps/shell/ls_shell_nav.c */
#include "ls_test.h"
#include "shell/ls_shell_nav.h"

#include <stdbool.h>
#include <string.h>

/**/
/* Before this fix, LsApp::exitToLauncher was `return true;` and LsShell::goBack
   was `if (_current) _current->back();` - throwing away the app's return value.
   Between them the back contract and Files' visible root-up button both failed
   to return the user to HOME:

     - back on a plain app went through exitToLauncher, which returned true and
       did nothing, so the app stayed up;
     - the shell dropped the false a well-behaved back would use to hand the
       navigation back to the shell;
     - Files' Up button at the roots view called exitToLauncher directly, with
       the same no-op result.

   The shell now goes through ls_shell_nav.  The tests here drive the gateway
   with a counting hook and prove every one of those paths reaches HOME once,
   and only once, in the situations that warrant it. */

static int s_home_count;

static void go_home_counter(void *ctx)
{
    (void)ctx;
    s_home_count++;
}

static void wire_counter_hooks(void)
{
    s_home_count = 0;
    ls_shell_nav_hooks_t h = { go_home_counter, NULL, NULL };
    ls_shell_nav_configure(&h);
}

/* --- exitToLauncher ------------------------------------------------------ */

LS_CASE(exit_launcher_from_current_non_home_app_goes_home)
{

    wire_counter_hooks();
    LS_CHECK(ls_shell_nav_exit_to_launcher(true, "P25"));
    LS_EQ_INT(s_home_count, 1);
}

LS_CASE(exit_launcher_from_backgrounded_app_is_ignored)
{
    /* An app the shell has already paused is not the current visible one -
       it must not be able to yank the user off whichever app is on screen. */
    wire_counter_hooks();
    LS_CHECK(!ls_shell_nav_exit_to_launcher(false, "P25"));
    LS_EQ_INT(s_home_count, 0);
}

LS_CASE(exit_launcher_from_home_does_not_recurse)
{
    /* HOME's own back() ends up here.  Launching HOME on top of itself would
       recurse through LsShell::launch, which is the recursion the task file
       calls out.  The default is_home_app predicate matches the literal
       registered name, which is what AppHome uses. */
    wire_counter_hooks();
    LS_CHECK(!ls_shell_nav_exit_to_launcher(true, "HOME"));
    LS_EQ_INT(s_home_count, 0);
}

static bool is_launcher_predicate(const char *name)
{
    return name && strcmp(name, "LAUNCHER") == 0;
}

LS_CASE(exit_launcher_uses_custom_is_home_predicate)
{
    /* Prove the seam is honoured: with a custom is_home_app predicate the
       gateway trusts the hook over the literal-"HOME" default.  This is the
       guardrail that lets a future rename of the HOME app stay in sync
       across the shell and this module. */
    wire_counter_hooks();

    /* Default predicate: "LAUNCHER" is not HOME, so navigation runs. */
    LS_CHECK(ls_shell_nav_exit_to_launcher(true, "LAUNCHER"));
    LS_EQ_INT(s_home_count, 1);

    /* Install a predicate that flips the classification. */
    ls_shell_nav_hooks_t h = { go_home_counter, is_launcher_predicate, NULL };
    ls_shell_nav_configure(&h);
    s_home_count = 0;

    /* Now "LAUNCHER" IS HOME - must be skipped. */
    LS_CHECK(!ls_shell_nav_exit_to_launcher(true, "LAUNCHER"));
    LS_EQ_INT(s_home_count, 0);

    /* And the literal "HOME" is no longer special. */
    LS_CHECK(ls_shell_nav_exit_to_launcher(true, "HOME"));
    LS_EQ_INT(s_home_count, 1);
}

/* --- goBack dispatch ----------------------------------------------------- */

LS_CASE(goback_handled_stays_put)
{
    /* Apps that consumed the back internally (modal close, tab pop) return
       true.  The shell must not double-navigate to HOME in that case. */
    wire_counter_hooks();
    LS_CHECK(!ls_shell_nav_dispatch_back(true));
    LS_EQ_INT(s_home_count, 0);
}

LS_CASE(goback_unhandled_reaches_home)
{
    /* The core "back contract" case: an app that returns false is saying
       "I did not handle it, shell please take over".  Old goBack dropped
       that on the floor. */
    wire_counter_hooks();
    LS_CHECK(ls_shell_nav_dispatch_back(false));
    LS_EQ_INT(s_home_count, 1);
}

LS_CASE(goback_with_no_hook_reports_no_nav)
{
    /* Before begin() has installed the shell hook, dispatch must be silent
       and honest - report false so callers do not believe navigation ran. */
    ls_shell_nav_configure(NULL);
    LS_CHECK(!ls_shell_nav_dispatch_back(false));
}

/* --- Files' root-up predicate ------------------------------------------- */

LS_CASE(fb_up_at_roots_is_home)
{
    /* Empty cwd is the roots-view sentinel FileBrowser uses.  Both back()
       and onBackButtonClicked route through this predicate now, so they
       cannot disagree about what "at the roots" means. */
    LS_CHECK(ls_shell_nav_fb_up_is_home(""));
    LS_CHECK(ls_shell_nav_fb_up_is_home(NULL));
}

LS_CASE(fb_up_inside_tree_pops_a_component)
{
    /* Anything with a real path is inside the tree - the caller must pop
       a component itself and NOT navigate HOME. */
    LS_CHECK(!ls_shell_nav_fb_up_is_home("/sdcard"));
    LS_CHECK(!ls_shell_nav_fb_up_is_home("/sdcard/music"));
    LS_CHECK(!ls_shell_nav_fb_up_is_home("/spiffs/logs/foo.txt"));
}

/* --- end-to-end walk of the paths the task calls out ------------------- */

LS_CASE(root_up_button_reaches_home_via_exit_to_launcher)
{
    /* onBackButtonClicked at the roots -> ls_shell_nav_fb_up_is_home("") ->
       exitToLauncher on the current app -> HOME.  Prove the composition. */
    wire_counter_hooks();
    const char *cwd = "";
    if (ls_shell_nav_fb_up_is_home(cwd)) {
        (void)ls_shell_nav_exit_to_launcher(true, "Files");
    }
    LS_EQ_INT(s_home_count, 1);
}

LS_CASE(root_up_inside_tree_does_not_reach_home)
{
    /* Same composition on a non-roots path: the browser navigates within
       the tree and the shell does not touch HOME. */
    wire_counter_hooks();
    const char *cwd = "/sdcard/music";
    if (ls_shell_nav_fb_up_is_home(cwd)) {
        (void)ls_shell_nav_exit_to_launcher(true, "Files");
    }
    LS_EQ_INT(s_home_count, 0);
}

LS_CASE(goback_on_app_that_delegates_reaches_home)
{
    /* Model of the ADS-B / Map / Media / Settings back(): they all just
       `return exitToLauncher();`.  In the new world back() returns true
       (exitToLauncher always does) but the launch itself was performed
       inside exitToLauncher.  Prove exactly one HOME nav. */
    wire_counter_hooks();
    /* App's back(): navigates then returns true. */
    (void)ls_shell_nav_exit_to_launcher(true, "ADS-B");
    const bool handled = true;
    /* Shell then dispatches with the return value - must NOT re-launch. */
    (void)ls_shell_nav_dispatch_back(handled);
    LS_EQ_INT(s_home_count, 1);
}

LS_CASE(goback_on_app_whose_back_returns_false_still_reaches_home)
{

    wire_counter_hooks();
    const bool handled = false;
    (void)ls_shell_nav_dispatch_back(handled);
    LS_EQ_INT(s_home_count, 1);
}
