/* Select the installed frontend ABI before loading any compiler libraries. */
#include <config.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "util.h"

const char *rs_program_name = "distcc-clang";

static int driver_major(const char *compiler, unsigned *major, int close_fd)
{
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (!pid) {
        close(fds[0]);
        if (close_fd > STDOUT_FILENO) close(close_fd);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(1);
        close(fds[1]);
        execl(compiler, compiler, "-dumpversion", (char *)NULL);
        _exit(1);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    FILE *stream = fdopen(fds[0], "r");
    char line[128];
    int ok = stream && fgets(line, sizeof(line), stream) && isdigit((unsigned char)line[0]);
    if (stream) fclose(stream); else close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    if (!ok || !WIFEXITED(status) || WEXITSTATUS(status)) return -1;
    char *end;
    unsigned long value = strtoul(line, &end, 10);
    if (value > UINT_MAX || (*end && *end != '.' && *end != '\n')) return -1;
    *major = value;
    return 0;
}

int main(int argc, char **argv)
{
    int native = argc == 4 && !strcmp(argv[1], "--native-worker");
    int index;
    if (native || (argc == 4 && !strcmp(argv[1], "--worker"))) index = 3;
    else if (argc >= 6 && !strcmp(argv[4], "preprocessed")) index = 5;
    else if (argc >= 8 && !strcmp(argv[4], "remote")) index = 7;
    else return 1;

    char *name = argv[index], *resolved = NULL;
    if (!strchr(name, '/')) {
        if (dcc_which(name, &resolved) || !resolved) return 1;
        argv[index] = resolved;
    }
    const char *base = dcc_find_basename(name);
    unsigned major;
    regex_t regex;
    regmatch_t match[4];
    if (regcomp(&regex, "(^|-)clang(\\+\\+)?-([0-9]+)(\\.[0-9]+)*$", REG_EXTENDED)) return 1;
    int tagged = !native && !regexec(&regex, base, 4, match, 0);
    regfree(&regex);
    if (tagged) {
        unsigned long value = strtoul(base + match[3].rm_so, NULL, 10);
        if (value > UINT_MAX) return 1;
        major = value;
    } else if (driver_major(argv[index], &major, index == 3 ? atoi(argv[2]) : -1)) return 1;

    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path));
    if (n <= 0 || n >= (ssize_t)sizeof(path)) return 1;
    path[n] = 0;
    char *file = strrchr(path, '/') + 1;
    size_t room = sizeof(path) - (file - path);
    if ((size_t)snprintf(file, room, "distcc-clang-%u", major) >= room) return 1;
    struct stat st;
    if (stat(path, &st)) {
        if (native && errno == ENOENT) return write(atoi(argv[2]), "U", 1) == 1 ? 0 : 1;
        perror(path);
        return 1;
    }
    execv(path, argv);
    perror(path);
    return 1;
}
