/* Real Files-owner canary client; only read-only input open is redirected.
 * Output, metadata, loader and directory operations remain native. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static int channel = -1;
static const char *root;
static int (*native_open)(const char *, int, ...);
static FILE *(*native_fopen)(const char *, const char *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
__attribute__((constructor)) static void initialize(void) {
    native_open = dlsym(RTLD_NEXT, "open");
    native_fopen = dlsym(RTLD_NEXT, "fopen");
    if (!native_open || !native_fopen) _exit(120);
    char image[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", image, sizeof(image)-1);
    if (size < 0) _exit(121);
    image[size] = 0;
    const char *expected = getenv("DCC_CANARY_IMAGE");
    if (expected && !strcmp(image, expected)) {
        channel = atoi(getenv("DCC_CANARY_FD"));
        root = getenv("DCC_CANARY_ROOT");
    }
}
static int input(const char *path) {
    size_t length = root ? strlen(root) : 0;
    return channel >= 0 && !strncmp(path, root, length) &&
        (path[length] == '/' || !path[length]);
}
static int request(const char *path, int flags) {
    int error = 0, fd = -1;
    pthread_mutex_lock(&lock);
    if (send(channel, path, strlen(path), MSG_NOSIGNAL) != (ssize_t)strlen(path)) error = EIO;
    else {
        struct iovec part = {&error, sizeof(error)};
        union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control;
        struct msghdr message = {.msg_iov=&part, .msg_iovlen=1,
            .msg_control=control.data, .msg_controllen=sizeof(control.data)};
        ssize_t size = recvmsg(channel, &message, MSG_CMSG_CLOEXEC);
        if (size != sizeof(error) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) error = EIO;
        else for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message,c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(fd)))
                memcpy(&fd, CMSG_DATA(c), sizeof(fd));
        if (!error && fd < 0) error = EPROTO;
    }
    pthread_mutex_unlock(&lock);
    if (error) { if (fd >= 0) close(fd); errno = error; return -1; }
    if (!(flags & O_CLOEXEC) && fcntl(fd, F_SETFD, 0)) { error = errno; close(fd); errno = error; return -1; }
    return fd;
}
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args,flags); mode = va_arg(args,unsigned); va_end(args);
    }
    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & O_CREAT) &&
        (flags & O_TMPFILE) != O_TMPFILE && input(path)) return request(path, flags);
    return native_open(path, flags, mode);
}
FILE *fopen(const char *path, const char *mode) {
    if (mode[0] != 'r' || strchr(mode, '+') || !input(path)) return native_fopen(path,mode);
    int fd = request(path, strchr(mode, 'e') ? O_CLOEXEC : 0);
    if (fd < 0) return NULL;
    FILE *result = fdopen(fd,mode);
    if (!result) { int error = errno; close(fd); errno = error; }
    return result;
}
