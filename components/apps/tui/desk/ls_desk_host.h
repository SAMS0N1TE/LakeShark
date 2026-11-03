/* Negotiated desk1 host services. No app pointers cross the transport. */
#ifndef LS_DESK_HOST_H
#define LS_DESK_HOST_H
#include <stddef.h>
#include <stdint.h>
#define LS_DESK_HOST_MAX 65536u
enum { LS_DESK_URL=1, LS_DESK_SAVE=2 };
void ls_desk_host_connect(uint32_t features, unsigned scale);
void ls_desk_host_disconnect(void);
void ls_desk_host_scale(unsigned scale);
void ls_desk_host_update(uint32_t features, unsigned scale);
uint32_t ls_desk_host_features(unsigned *scale);
int ls_desk_host_begin(int op, const char *name);
int ls_desk_host_payload(int id, const void *bytes, size_t n);
int ls_desk_host_status(int id, int consume);
void ls_desk_host_cancel(int id);
void ls_desk_host_reply(int id, int result);
/* One bounded text record; called by the terminal worker, never an app. */
int ls_desk_host_line(char *line, size_t cap);
#endif
