/* Real Files-owner canary client. fstat and FD operations remain native. */
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
#include "owner.h"
static int channel = -1;
static const char *root;
static int (*native_open)(const char *, int, ...);
static FILE *(*native_fopen)(const char *, const char *);
static int (*native_stat)(const char *, struct stat *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
__attribute__((constructor)) static void initialize(void) {
    native_open = dlsym(RTLD_NEXT, "open");
    native_fopen = dlsym(RTLD_NEXT, "fopen");
    native_stat = dlsym(RTLD_NEXT, "stat");
    if (!native_open || !native_fopen || !native_stat) _exit(120);
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
static int request(const char *path, int flags, struct stat *attributes) {
    int error = 0, fd = -1;
    char operation = attributes ? 'S' : 'O';
    struct iovec query[] = {{&operation, 1}, {(void *)path, strlen(path)}};
    struct msghdr outgoing = {.msg_iov=query, .msg_iovlen=2};
    pthread_mutex_lock(&lock);
    if (sendmsg(channel, &outgoing, MSG_NOSIGNAL) != (ssize_t)strlen(path)+1) error = EIO;
    else {
        struct owner_reply reply;
        struct iovec part = {&reply, sizeof(reply)};
        union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control;
        struct msghdr message = {.msg_iov=&part, .msg_iovlen=1,
            .msg_control=control.data, .msg_controllen=sizeof(control.data)};
        ssize_t size = recvmsg(channel, &message, MSG_CMSG_CLOEXEC);
        if (size != sizeof(reply) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) error = EIO;
        else {
            error = reply.error;
            if (attributes && !error) *attributes = reply.attributes;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message,c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(fd)))
                memcpy(&fd, CMSG_DATA(c), sizeof(fd));
        if (!error && (attributes ? fd >= 0 : fd < 0)) error = EPROTO;
    }
    pthread_mutex_unlock(&lock);
    if (error) { if (fd >= 0) close(fd); errno = error; return -1; }
    if (attributes) return 0;
    if (!(flags & O_CLOEXEC) && fcntl(fd, F_SETFD, 0)) { error = errno; close(fd); errno = error; return -1; }
    return fd;
}
int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args,flags); mode = va_arg(args,unsigned); va_end(args);
    }
    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & O_CREAT) &&
        (flags & O_TMPFILE) != O_TMPFILE && input(path)) return request(path, flags, NULL);
    return native_open(path, flags, mode);
}
FILE *fopen(const char *path, const char *mode) {
    if (mode[0] != 'r' || strchr(mode, '+') || !input(path)) return native_fopen(path,mode);
    int fd = request(path, strchr(mode, 'e') ? O_CLOEXEC : 0, NULL);
    if (fd < 0) return NULL;
    FILE *result = fdopen(fd,mode);
    if (!result) { int error = errno; close(fd); errno = error; }
    return result;
}
int stat(const char *path, struct stat *attributes) {
    return input(path) ? request(path, 0, attributes) : native_stat(path, attributes);
}
