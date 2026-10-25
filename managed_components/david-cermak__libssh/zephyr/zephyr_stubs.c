//
// Created by david on 2/5/26.
//
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

// Dummy implementation of libssh required functions

uid_t getuid(void)
{
	return 0;  // Return fake UID
}

struct passwd *getpwnam(const char *name)
{
	return NULL;
}

pid_t waitpid(pid_t pid, int *wstatus, int options)
{
	return -1;  // Simulate failure
}
