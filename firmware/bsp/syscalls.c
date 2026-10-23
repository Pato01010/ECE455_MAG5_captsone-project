/* syscalls.c - the four file-system hooks newlib links against. Nothing here uses files: these
 * replace libnosys's versions only so the link stays free of "not implemented" warnings.
 */
#include <errno.h>

int _close(int fd);
int _lseek(int fd, int offset, int whence);
int _read(int fd, char *buf, int n);
int _write(int fd, const char *buf, int n);

int _close(int fd) { (void)fd; errno = ENOSYS; return -1; }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; errno = ENOSYS; return -1; }
int _read(int fd, char *buf, int n) { (void)fd; (void)buf; (void)n; errno = ENOSYS; return -1; }
int _write(int fd, const char *buf, int n) { (void)fd; (void)buf; (void)n; errno = ENOSYS; return -1; }
