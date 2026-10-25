#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

// Dummy implementation of libssh required functions

uid_t getuid(void)
{
    return 0;  // Return fake UID
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result)
{
    if (result) {
        *result = NULL;
    }
    return -1;  // Simulate failure
}

struct passwd *getpwnam(const char *name)
{
    return NULL;
}

pid_t waitpid(pid_t pid, int *wstatus, int options)
{
    return -1;  // Simulate failure
}
