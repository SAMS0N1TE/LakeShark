#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

#define	F_OK	0
#define	R_OK	4
#define	W_OK	2
#define	X_OK	1
#define STDIN_FILENO    0       /* standard input file descriptor */
#ifndef O_NDELAY
#define O_NDELAY    O_NONBLOCK /* same as O_NONBLOCK, for compatibility */
#endif
#define	S_IWRITE 	0000200	/* write permission, owner */
#define F_SETFD		2	/* set f_flags */

uid_t getuid(void);
int	isatty (int __fildes);
char 	*strsep (char **, const char *);

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);

struct passwd *getpwnam(const char *name);

pid_t waitpid(pid_t pid, int *wstatus, int options);

int access(const char *path, int amode);
