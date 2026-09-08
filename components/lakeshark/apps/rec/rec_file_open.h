#ifndef REC_FILE_OPEN_H
#define REC_FILE_OPEN_H

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/*LS-914  Exclusive creation must be atomic on both the device and the host
   bench. The MinGW MSVCRT runtime rejects fopen("wx"); O_EXCL provides the
   same no-overwrite contract without depending on the C11 mode extension. */
static inline FILE *rec_file_open_new(const char *path)
{
    if (!path) return NULL;
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL;
    FILE *file = _fdopen(fd, "wb");
    if (!file) _close(fd);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "wb");
    if (!file) close(fd);
#endif
    return file;
}
#endif
