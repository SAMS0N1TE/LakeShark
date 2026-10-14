#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void ls_c6_publish(void);
/* 'c6 update' and 'c6 status', reached through the one 'c6' console command. */
int ls_c6_update_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
