/* ls_err_name: the names the console prints with IDF's table compiled out. */
#include "ls_err_name.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char b[16];
    assert(!strcmp(ls_err_name(0, b, sizeof(b)), "ESP_OK"));            /* the line that said UNKNOWN ERROR */
    assert(!strcmp(ls_err_name(-1, b, sizeof(b)), "ESP_FAIL"));
    assert(!strcmp(ls_err_name(0x101, b, sizeof(b)), "ESP_ERR_NO_MEM"));
    assert(!strcmp(ls_err_name(0x107, b, sizeof(b)), "ESP_ERR_TIMEOUT"));
    assert(!strcmp(ls_err_name(0x1102, b, sizeof(b)), "ESP_ERR_NVS_NOT_FOUND"));
    const char *u = ls_err_name(0x3007, b, sizeof(b));
    assert(u == b && !strcmp(u, "ERR 0x3007"));
    assert(!strcmp(ls_err_name(-2, b, sizeof(b)), "ERR 0xfffffffe"));
    char tiny[4];
    assert(!strcmp(ls_err_name(0x3007, tiny, sizeof(tiny)), "ERR"));     /* truncated, still terminated */
    puts("test_ls_err_name: ok");
    return 0;
}
