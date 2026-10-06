/* LS_TEST_SOURCES: ls_value.c and ls_action.c alone */
/* The value and action tables when they are full. Both used to refuse
   silently, and 2.8.2's one new value pushed sys.volume off the end: the
   volume controls showed no number and nothing said why. */

#include "ls_test.h"

#include "ls_action.h"
#include "ls_value.h"

#include <stdio.h>

static bool v_one(ls_val_t *o) { o->kind = LS_VAL_INT; o->i = 1; return true; }
static bool v_two(ls_val_t *o) { o->kind = LS_VAL_INT; o->i = 2; return true; }

static ls_act_status_t a_one(const ls_args_t *in, ls_val_t *out)
{ (void)in; out->kind = LS_VAL_INT; out->i = 1; return LS_ACT_OK; }
static ls_act_status_t a_two(const ls_args_t *in, ls_val_t *out)
{ (void)in; out->kind = LS_VAL_INT; out->i = 2; return LS_ACT_OK; }

/* The tables keep the pointer, so the names must outlive them. */
static char s_names[LS_VALUE_MAX + LS_ACTION_MAX][16];
static int  s_named;

static const char *name(const char *stem, int i)
{
    char *n = s_names[s_named++];
    snprintf(n, sizeof(s_names[0]), "%s.%d", stem, i);
    return n;
}

/* Every case fills its table first, so none depends on the order the
   runner picks. */
static void fill_values(void)
{
    for (int i = ls_value_count(); i < LS_VALUE_MAX; i++)
        LS_CHECK(ls_value_publish(name("v", i), NULL, v_one));
    LS_EQ_INT(ls_value_count(), LS_VALUE_MAX);
}

static void fill_actions(void)
{
    for (int i = ls_action_count(); i < LS_ACTION_MAX; i++)
        LS_CHECK(ls_action_register(name("a", i), "", LS_CAP_UI, a_one, "t"));
    LS_EQ_INT(ls_action_count(), LS_ACTION_MAX);
}

LS_CASE(a_full_value_table_refuses_the_next_and_counts_it)
{
    fill_values();
    const int was = ls_value_refused();

    LS_CHECK(!ls_value_publish("v.late", NULL, v_one));
    LS_EQ_INT(ls_value_refused(), was + 1);
    LS_EQ_INT(ls_value_count(), LS_VALUE_MAX);

    ls_val_t v;
    LS_CHECK(!ls_value_read("v.late", &v, NULL));
}

LS_CASE(a_known_value_is_replaced_even_on_a_full_table)
{
    fill_values();
    const int was = ls_value_refused();
    const char *first = ls_value_name(0);

    LS_CHECK(ls_value_publish(first, NULL, v_two));
    ls_val_t v;
    LS_CHECK(ls_value_read(first, &v, NULL));
    LS_EQ_INT(v.i, 2);
    LS_EQ_INT(ls_value_refused(), was);
}

LS_CASE(a_full_action_table_refuses_the_next_and_counts_it)
{
    fill_actions();
    const int was = ls_action_refused();

    LS_CHECK(!ls_action_register("a.late", "", LS_CAP_UI, a_one, "t"));
    LS_EQ_INT(ls_action_refused(), was + 1);
    LS_EQ_INT(ls_action_count(), LS_ACTION_MAX);
}

LS_CASE(a_known_action_is_replaced_even_on_a_full_table)
{
    fill_actions();
    const int was = ls_action_refused();
    const char *first = ls_action_name(0);

    LS_CHECK(ls_action_register(first, "", LS_CAP_UI, a_two, "t"));
    ls_args_t in = { 0 };
    ls_val_t out;
    LS_EQ_INT(ls_action_call(first, &in, &out, LS_CAP_UI), LS_ACT_OK);
    LS_EQ_INT(out.i, 2);
    LS_EQ_INT(ls_action_refused(), was);
}
