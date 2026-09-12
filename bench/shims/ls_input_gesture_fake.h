#ifndef LS_INPUT_GESTURE_FAKE_H
#define LS_INPUT_GESTURE_FAKE_H

#include <stdbool.h>

#include "shell/ls_input_gesture.h"

void ls_input_gesture_fake_set(bool available,
                               ls_input_gesture_id_t gesture);

#endif
