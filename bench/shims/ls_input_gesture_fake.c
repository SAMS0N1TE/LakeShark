#include "ls_input_gesture_fake.h"

static bool s_available;
static ls_input_gesture_id_t s_gesture;

void ls_input_gesture_fake_set(bool available,
                               ls_input_gesture_id_t gesture)
{
    s_available = available;
    s_gesture = gesture;
}

bool ls_input_local_gesture(ls_input_gesture_id_t *out)
{
    if (!out || !s_available) return false;
    *out = s_gesture;
    s_available = false;
    return true;
}
