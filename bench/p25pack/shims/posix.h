/* MinGW uses the one-argument mkdir; firmware uses POSIX/FAT VFS. */
#pragma once
#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
#endif
