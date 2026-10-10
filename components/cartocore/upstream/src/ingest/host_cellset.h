#ifndef CC_HOST_CELLSET_H
#define CC_HOST_CELLSET_H
#include "cartocore/cellset.h"
#include <stdio.h>
typedef struct { cc_cellset set; FILE *index; char root[1024]; void *directory,*staging; } cc_host_cellset;
int cc_host_is_directory(const char *path);
int cc_host_cellset_open(cc_host_cellset *host,const char *root);
void cc_host_cellset_close(cc_host_cellset *host);
int cc_build_cells_cli(int argc,char **argv);
#endif
