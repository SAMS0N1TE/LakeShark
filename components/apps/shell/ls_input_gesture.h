#ifndef LS_INPUT_GESTURE_H
#define LS_INPUT_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_INPUT_GESTURE_NONE = 0,
    LS_INPUT_GESTURE_LOCAL_TOUCH = 1,
    LS_INPUT_GESTURE_LOCAL_KEY = 2,
    LS_INPUT_GESTURE_REMOTE = 3,
} ls_input_gesture_source_t;

typedef struct {
    uint64_t sequence;
    ls_input_gesture_source_t source;
} ls_input_gesture_id_t;

/* Valid only while handling the active LVGL input event. */
bool ls_input_local_gesture(ls_input_gesture_id_t *out);

#ifdef __cplusplus
}
#endif

#endif
