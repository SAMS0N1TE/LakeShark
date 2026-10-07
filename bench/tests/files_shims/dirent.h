#ifndef FILES_TEST_DIRENT_H
#define FILES_TEST_DIRENT_H
#define DT_DIR 4
typedef struct { int cursor; } DIR;
struct dirent { char d_name[256]; unsigned char d_type; };
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif
