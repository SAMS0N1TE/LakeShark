
#ifndef EVENT_STREAM_H
#define EVENT_STREAM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void event_stream_init(void);

void event_stream_set_enabled(bool en);
bool event_stream_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
