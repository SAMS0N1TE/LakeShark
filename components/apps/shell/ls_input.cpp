#include "shell/ls_input.h"
#include "shell/ls_keymap.h"

#include "ls_board.h"

#if LS_HAS_KEYBOARD
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/*LS-110  LVGL previously had only a pointer indev and no group, so a board
  without touch could not focus or activate any widget.  Keep key translation
  outside the board driver, queue task-context events here, and rebuild the
  group from only the visible screen tree whenever the shell changes apps. */
static_assert(int(LS_KEY_ACTION_HOME) == int(LV_KEY_HOME), "key map drift");
static_assert(int(LS_KEY_ACTION_END) == int(LV_KEY_END), "key map drift");
static_assert(int(LS_KEY_ACTION_BACKSPACE) == int(LV_KEY_BACKSPACE), "key map drift");
static_assert(int(LS_KEY_ACTION_NEXT) == int(LV_KEY_NEXT), "key map drift");
static_assert(int(LS_KEY_ACTION_ENTER) == int(LV_KEY_ENTER), "key map drift");
static_assert(int(LS_KEY_ACTION_PREV) == int(LV_KEY_PREV), "key map drift");
static_assert(int(LS_KEY_ACTION_RIGHT) == int(LV_KEY_RIGHT), "key map drift");
static_assert(int(LS_KEY_ACTION_LEFT) == int(LV_KEY_LEFT), "key map drift");
static_assert(int(LS_KEY_ACTION_ESCAPE) == int(LV_KEY_ESC), "key map drift");
static_assert(int(LS_KEY_ACTION_DELETE) == int(LV_KEY_DEL), "key map drift");

struct key_event_t {
    uint32_t key;
    lv_indev_state_t state;
};

static constexpr unsigned KEY_QUEUE_LEN = 32;
static key_event_t s_queue[KEY_QUEUE_LEN];
static unsigned s_head;
static unsigned s_tail;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_indev_state_t s_last_state = LV_INDEV_STATE_RELEASED;
static uint32_t s_last_key;
static lv_indev_drv_t s_drv;
static lv_indev_t *s_indev;
static lv_group_t *s_group;
static lv_obj_t *s_screen;
static lv_obj_t *s_focus_root;

static void focus_tree(lv_obj_t *obj)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_is_group_def(obj)) lv_group_add_obj(s_group, obj);

    const uint32_t count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < count; ++i) focus_tree(lv_obj_get_child(obj, i));
}

static void rebuild_focus(void)
{
    if (!s_group) return;
    lv_group_remove_all_objs(s_group);
    focus_tree(s_focus_root ? s_focus_root : s_screen);
}

static void keypad_read(lv_indev_drv_t *, lv_indev_data_t *data)
{
    bool have_event = false;
    key_event_t event = {};

    portENTER_CRITICAL(&s_lock);
    if (s_tail != s_head) {
        event = s_queue[s_tail];
        s_tail = (s_tail + 1U) % KEY_QUEUE_LEN;
        have_event = true;
    }
    data->continue_reading = (s_tail != s_head);
    portEXIT_CRITICAL(&s_lock);

    if (have_event) {
        s_last_key = event.key;
        s_last_state = event.state;
    }
    data->key = s_last_key;
    data->state = s_last_state;
}
#endif

static uint64_t s_gesture_sequence;

void ls_input_init(void)
{
#if LS_HAS_KEYBOARD
    if (s_indev) return;

    s_group = lv_group_create();
    if (!s_group) return;
    lv_group_set_default(s_group);

    lv_indev_drv_init(&s_drv);
    s_drv.type = LV_INDEV_TYPE_KEYPAD;
    s_drv.read_cb = keypad_read;
    s_indev = lv_indev_drv_register(&s_drv);
    if (s_indev) lv_indev_set_group(s_indev, s_group);
#endif
}

void ls_input_set_screen(lv_obj_t *screen)
{
#if LS_HAS_KEYBOARD
    s_screen = screen;
    if (!s_focus_root) rebuild_focus();
#else
    (void)screen;
#endif
}

void ls_input_refresh_focus(void)
{
#if LS_HAS_KEYBOARD
    rebuild_focus();
#endif
}

void ls_input_focus_modal(lv_obj_t *modal)
{
#if LS_HAS_KEYBOARD
    s_focus_root = modal;
    rebuild_focus();
#else
    (void)modal;
#endif
}

void ls_input_focus_screen(void)
{
#if LS_HAS_KEYBOARD
    s_focus_root = nullptr;
    rebuild_focus();
#endif
}

bool ls_input_key_event(uint32_t key, bool pressed)
{
#if LS_HAS_KEYBOARD
    const uint32_t action = ls_keymap_action(key);
    if (!action || !s_indev) return false;

    bool queued = false;
    portENTER_CRITICAL(&s_lock);
    const unsigned next = (s_head + 1U) % KEY_QUEUE_LEN;
    if (next != s_tail) {
        s_queue[s_head] = {action, pressed ? LV_INDEV_STATE_PRESSED
                                          : LV_INDEV_STATE_RELEASED};
        s_head = next;
        queued = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return queued;
#else
    (void)key;
    (void)pressed;
    return false;
#endif
}

bool ls_input_local_gesture(ls_input_gesture_id_t *out)
{
    if (!out) return false;
    *out = {};
#if LS_USE_DISPLAY
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    const lv_indev_type_t type = lv_indev_get_type(indev);
    if (type == LV_INDEV_TYPE_POINTER || type == LV_INDEV_TYPE_BUTTON)
        out->source = LS_INPUT_GESTURE_LOCAL_TOUCH;
    else if (type == LV_INDEV_TYPE_KEYPAD)
        out->source = LS_INPUT_GESTURE_LOCAL_KEY;
    else
        return false;
    out->sequence = __atomic_add_fetch(&s_gesture_sequence, UINT64_C(1),
                                       __ATOMIC_RELAXED);
    if (out->sequence == 0)
        out->sequence = __atomic_add_fetch(&s_gesture_sequence, UINT64_C(1),
                                           __ATOMIC_RELAXED);
    return true;
#else
    return false;
#endif
}
