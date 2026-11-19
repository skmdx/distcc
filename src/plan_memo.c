/* Driver plan templates and filesystem dependency journals. */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "distcc.h"
#include "exec.h"
#include "util.h"
#include "native_worker.h"
#include "broker.h"
#include "plan_memo.h"
#include "trace.h"
#ifdef __linux__
#include <sys/inotify.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#endif


void dcc_free_plan_template(struct dcc_plan_template *plan)
{
    if (plan->argv) dcc_free_argv(plan->argv);
    for (size_t i = 0; i < plan->count; ++i) free(plan->values[i]);
    free(plan->values);
    *plan = (struct dcc_plan_template){0};
}

/* Punctuation stays in the shell-free driver template. In particular Clang
 * quotes whitespace, dollars, hashes and backslashes itself when lowering -MQ. */
static int template_part(struct dcc_plan_template *plan, char **arg, size_t first, size_t last)
{
    static const char safe[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    char *text = NULL;
    size_t length;
    FILE *stream = open_memstream(&text, &length);
    if (!stream) return -1;
    fwrite(*arg, 1, first, stream);
    int ok = 1;
    while (first < last) {
        size_t span = strspn(*arg + first, safe);
        if (!span) { fputc((*arg)[first++], stream); continue; }
        if (span > last-first) span = last-first;
        if (plan->count == plan->capacity) {
            size_t capacity = plan->capacity ? plan->capacity * 2 : 8;
            char **values = reallocarray(plan->values, capacity, sizeof(*values));
            if (!values) { ok = 0; break; }
            plan->values = values;
            plan->capacity = capacity;
        }
        char *value = strndup(*arg + first, span);
        if (!value) { ok = 0; break; }
        fprintf(stream, DCC_PLAN_MARKER "%zu__", plan->count);
        plan->values[plan->count++] = value;
        first += span;
    }
    fputs(*arg + last, stream);
    if (fclose(stream) || !ok) { free(text); return -1; }
    free(*arg);
    *arg = text;
    return 0;
}

void dcc_clang_plan_template(struct dcc_plan_template *plan, char **argv, const char *input)
{
    const char *extension = dcc_find_extension_const(input);
    if (!extension || access(input, R_OK)) return;
    size_t inputs = 0;
    extern char **environ;
    for (char **env = environ; *env; ++env)
        if (strstr(*env, DCC_PLAN_MARKER)) return;
    for (size_t i = 0; argv[i]; ++i) {
        if (strstr(argv[i], DCC_PLAN_MARKER) || !strncmp(argv[i], "--config", 8) ||
            !strcmp(argv[i], "-Xclang") ||
            !strcmp(argv[i], "-Xpreprocessor") || !strcmp(argv[i], "-Xassembler") ||
            strstr(argv[i], "record-command-line") || !strcmp(argv[i], "-MJ")) return;
        inputs += !strcmp(argv[i], input);
    }
    if (inputs != 1 || dcc_copy_argv(argv, &plan->argv, 0)) return;
    for (size_t i = 1; argv[i]; ++i) {
        const char *arg = argv[i];
        size_t first = 0, last = strlen(arg);
        if (!strcmp(arg, input)) {
            first = dcc_find_basename(arg) - arg;
            last = extension - input;
        } else {
            unsigned flag = !strncmp(arg,"-MF",3) || !strncmp(arg,"-MT",3) || !strncmp(arg,"-MQ",3) ? 3 :
                            !strncmp(arg,"-o",2) ? 2 : 0;
            if (!flag) continue;
            if (arg[flag]) first = flag;
            else { if (!argv[++i]) goto exact; arg = argv[i]; last = strlen(arg); }
            if (!strcmp(arg+first,"-") || !strcmp(arg+first,"/dev/null")) continue;
            if (flag == 2) {
                const char *suffix = dcc_find_extension_const(arg+first);
                if (suffix) last = suffix-arg;
            }
        }
        if (template_part(plan, &plan->argv[i], first, last)) goto exact;
    }
    return;
exact:
    dcc_free_plan_template(plan);
}

int dcc_bind_plan(const struct dcc_plan_template *plan, char **argv)
{
    for (char **p = argv; *p; ++p) {
        const char *start = *p, *marker = strstr(start, DCC_PLAN_MARKER);
        if (!marker) continue;
        char *text = NULL;
        size_t length;
        FILE *stream = open_memstream(&text, &length);
        if (!stream) return -1;
        do {
            fwrite(start, 1, marker-start, stream);
            const char *number = marker + sizeof(DCC_PLAN_MARKER)-1;
            char *end;
            unsigned long index = strtoul(number, &end, 10);
            if (end == number || strncmp(end,"__",2) || index >= plan->count) {
                fclose(stream); free(text); return -1;
            }
            fputs(plan->values[index], stream);
            start = end+2;
        } while ((marker = strstr(start,DCC_PLAN_MARKER)));
        fputs(start, stream);
        if (fclose(stream)) { free(text); return -1; }
        free(*p);
        *p = text;
    }
    return 0;
}

static int validate(const struct dcc_plan_dependency *d, const char *path, const char *target)
{
    struct stat st={0};
    int result=-1, error;
    switch (d->operation) {
    case DCC_PLAN_STAT: result=stat(path,&st); break;
    case DCC_PLAN_LSTAT: result=lstat(path,&st); break;
    case DCC_PLAN_ACCESS: result=access(path,d->flags); break;
    case DCC_PLAN_ACCESS_AT:
        result=faccessat(AT_FDCWD,path,d->flags & 7,d->flags & ~7U); break;
    case DCC_PLAN_OPEN: {
        if (strcmp(path,"/dev/null") && ((d->flags&O_ACCMODE)!=O_RDONLY ||
            d->flags&(O_CREAT|O_TRUNC) || (d->flags&O_TMPFILE)==O_TMPFILE)) return 0;
        int fd=open(path,d->flags|O_CLOEXEC);
        if (fd>=0) { result=fstat(fd,&st); error=errno; close(fd); errno=error; }
        break;
    }
    case DCC_PLAN_STREAM: {
        if (!target || ((*target!='r' || strchr(target,'+')) && strcmp(path,"/dev/null"))) return 0;
        FILE *file=fopen(path,target);
        if (file) { result=fstat(fileno(file),&st); error=errno; fclose(file); errno=error; }
        break;
    }
    case DCC_PLAN_DIRECTORY: {
        DIR *directory=opendir(path);
        if (directory) { result=fstat(dirfd(directory),&st); error=errno; closedir(directory); errno=error; }
        break;
    }
    case DCC_PLAN_LINK: {
        char buffer[PATH_MAX];
        ssize_t n=readlink(path,buffer,sizeof(buffer));
        if (n>=0) {
            if (!target || (size_t)n!=strlen(target) || memcmp(buffer,target,n)) return 0;
            result=lstat(path,&st);
        }
        break;
    }
    case DCC_PLAN_REALPATH: {
        char buffer[PATH_MAX];
        if (realpath(path,buffer)) {
            if (!target || strcmp(buffer,target)) return 0;
            result=stat(path,&st);
        }
        break;
    }
    default: return 0;
    }
    error=result ? errno : 0;
    return error==d->error && (error || d->operation==DCC_PLAN_ACCESS ||
        d->operation==DCC_PLAN_ACCESS_AT || dcc_plan_stat_equal(d->operation,&st,&d->metadata));
}

struct record { struct dcc_plan_dependency d; const char *path, *target; };
static int order(const void *aa, const void *bb)
{
    const struct record *a=aa,*b=bb;
    int diff=strcmp(a->path,b->path);
    if (diff) return diff;
    if (a->d.operation!=b->d.operation) return a->d.operation<b->d.operation ? -1 : 1;
    return a->d.flags<b->d.flags ? -1 : a->d.flags>b->d.flags;
}
static int parse_journal(const unsigned char *data, size_t length,
                           struct record **records, unsigned *count)
{
    if (length<4 || data[0]!=1 || data[1]!=1 || data[2] || data[3]) {
        if (length>=4) rs_trace("GCC plan journal incomplete: driver=%u planner=%u invalid=%u bytes=%zu",
                                data[0],data[1],data[2],length);
        return 0;
    }
    struct record *all=calloc(length/sizeof(struct dcc_plan_dependency)+1,sizeof(*all));
    if (!all) return 0;
    size_t at=4;
    unsigned used=0;
    while (at<length) {
        if (length-at<sizeof(all[used].d)) goto bad;
        struct record *r=&all[used++];
        memcpy(&r->d,data+at,sizeof(r->d)); at+=sizeof(r->d);
        if (!r->d.length || r->d.length>length-at) goto bad;
        r->path=(const char *)data+at; at+=r->d.length;
        if (*r->path!='/' || r->path[r->d.length-1] || strlen(r->path)!=r->d.length-1) goto bad;
        if (r->d.target_length) {
            if (r->d.target_length>length-at) goto bad;
            r->target=(const char *)data+at; at+=r->d.target_length;
            if (r->target[r->d.target_length-1] || strlen(r->target)!=r->d.target_length-1) goto bad;
        }
    }
    *records=all; *count=used; return 1;
bad:
    free(all); return 0;
}
static int journal_records(const unsigned char *data, size_t length, struct record **records, unsigned *count)
{
    struct record *all;
    unsigned used;
    if (!parse_journal(data, length, &all, &used)) return 0;
    qsort(all,used,sizeof(*all),order);
    for (unsigned i=0;i<used;++i) {
        if (i && !order(&all[i-1],&all[i])) {
            if (all[i].d.error!=all[i-1].d.error || !dcc_plan_stat_equal(all[i].d.operation,&all[i].d.metadata,&all[i-1].d.metadata) ||
                strcmp(all[i].target?all[i].target:"",all[i-1].target?all[i-1].target:"")) goto bad;
        } else if (!validate(&all[i].d,all[i].path,all[i].target)) {
            rs_trace("GCC plan dependency changed: operation=%u path=%s",all[i].d.operation,all[i].path);
            goto bad;
        }
    }
    *records=all; *count=used; return 1;
bad:
    free(all); return 0;
}

static unsigned char *file_bytes(int fd, size_t *size, size_t limit)
{
    struct stat st;
    if (fstat(fd,&st) || st.st_size<0 || (uint64_t)st.st_size>limit) return NULL;
    unsigned char *data=malloc(st.st_size+1);
    if (!data) return NULL;
    if (pread(fd,data,st.st_size,0)!=st.st_size) { free(data); return NULL; }
    *size=st.st_size; return data;
}

char *dcc_plan_memo_encode(int journal, int output, unsigned *dependencies, size_t *length)
{
    size_t size=0,plan_size=0;
    unsigned char *data=file_bytes(journal,&size,DCC_PLAN_JOURNAL_LIMIT), *plan=file_bytes(output,&plan_size,DCC_MEMO_LIMIT);
    struct record *records=NULL;
    unsigned count=0;
    char *memo=NULL;
    if (!data || !plan || !journal_records(data,size,&records,&count)) goto out;
    *dependencies=0;
    for (unsigned i=0;i<count;++i) if (!i || order(&records[i-1],&records[i]))
        ++*dependencies;
    size_t compact=4;
    for (unsigned i=0;i<count;++i) if (!i || order(&records[i-1],&records[i]))
        compact+=sizeof(records[i].d)+records[i].d.length+records[i].d.target_length;
    if (8+compact+plan_size+1>DCC_MEMO_LIMIT) goto out;
    unsigned char *packed=malloc(8+compact+plan_size);
    if (!packed) goto out;
    uint32_t lengths[]={(uint32_t)compact,(uint32_t)plan_size};
    memcpy(packed,lengths,8); memcpy(packed+8,data,4);
    size_t at=12;
    for (unsigned i=0;i<count;++i) if (!i || order(&records[i-1],&records[i])) {
        struct record *r=&records[i];
        memcpy(packed+at,&r->d,sizeof(r->d)); at+=sizeof(r->d);
        memcpy(packed+at,r->path,r->d.length); at+=r->d.length;
        if (r->target) { memcpy(packed+at,r->target,r->d.target_length); at+=r->d.target_length; }
    }
    memcpy(packed+at,plan,plan_size); at+=plan_size;
    memo=(char *)packed;
    *length=at;
out:
    free(records); free(data); free(plan); return memo;
}

int dcc_plan_memo_decode(const char *memo, size_t size, int output, unsigned *dependencies, int verified)
{
    if (size < 12 || size >= DCC_MEMO_LIMIT) return 0;
    const unsigned char *data = (const unsigned char *)memo;
    int valid=0;
    struct record *records=NULL;
    uint32_t lengths[2]; memcpy(lengths,data,8);
    if ((uint64_t)lengths[0]+lengths[1]+8!=size || lengths[0]>DCC_PLAN_JOURNAL_LIMIT ||
        (!verified && !journal_records(data+8,lengths[0],&records,dependencies))) goto out;
    if (verified) *dependencies = verified;
    if (ftruncate(output,0) || lseek(output,0,SEEK_SET)<0 ||
        write(output,data+8+lengths[0],lengths[1])!=(ssize_t)lengths[1]) goto out;
    valid=1;
out:
    free(records); return valid;
}

#ifdef __linux__
/* The kernel assigns one wd per inode on this instance, including aliases.
 * Each subscription also belongs to one memo, so eviction can unlink it
 * directly and remove the kernel watch when its last subscriber leaves. */
LIST_HEAD(watch_interests, watch_interest);
LIST_HEAD(watched_inodes, watched_inode);
struct watched_inode {
    int wd;
    LIST_ENTRY(watched_inode) bucket;
    struct watch_interests users;
};
struct watch_interest {
    struct watched_inode *inode;
    struct dcc_plan_watch *plan;
    LIST_ENTRY(watch_interest) by_inode, by_plan;
    int children;
    char name[];
};
struct dcc_plan_monitor {
    int fd;
    struct watched_inodes buckets[256];
    uid_t uid;
    gid_t gid;
    struct stat context[3];
    int ngroups;
    gid_t groups[]; /* Broker groups followed by scratch for one peer. */
};
struct dcc_plan_watch {
    struct dcc_plan_monitor *monitor;
    struct watch_interests paths;
    unsigned count; /* Zero once invalidated. */
    size_t bytes;
};

static int read_context(pid_t pid, struct stat *context)
{
    static const char *const paths[] = {"root", "ns/mnt", "ns/user"};
    for (unsigned i = 0; i < sizeof(paths)/sizeof(*paths); ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/%s", (long)pid, paths[i]);
        if (stat(path, &context[i])) return 0;
    }
    return 1;
}

struct dcc_plan_monitor *dcc_plan_monitor_new(int ep)
{
    int ngroups = getgroups(0, NULL);
    if (ngroups < 0 || getuid() != geteuid() || getgid() != getegid()) return NULL;
    struct dcc_plan_monitor *m = calloc(1, sizeof(*m) + (2*(size_t)ngroups+1)*sizeof(gid_t));
    if (!m) return NULL;
    m->uid = geteuid(); m->gid = getegid(); m->ngroups = ngroups;
    m->fd = -1;
    if (getgroups(ngroups, m->groups) != ngroups || !read_context(getpid(), m->context))
        goto failed;
    m->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = DCC_PLAN_WATCH_EVENT};
    if (m->fd >= 0 && !epoll_ctl(ep, EPOLL_CTL_ADD, m->fd, &event)) return m;
failed:
    dcc_plan_monitor_free(m);
    return NULL;
}

void dcc_plan_monitor_free(struct dcc_plan_monitor *m)
{
    if (!m) return;
    if (m->fd >= 0) close(m->fd);
    free(m);
}

int dcc_plan_peer_matches(struct dcc_plan_monitor *m, int socket, pid_t pid, uid_t uid, gid_t gid)
{
    struct stat peer[3];
    if (!m || m->uid != uid || m->gid != gid || !read_context(pid, peer)) return 0;
    for (unsigned i = 0; i < sizeof(peer)/sizeof(*peer); ++i)
        if (peer[i].st_dev != m->context[i].st_dev || peer[i].st_ino != m->context[i].st_ino) return 0;
    gid_t *groups = m->groups + m->ngroups;
    socklen_t bytes = (m->ngroups+1)*sizeof(*groups);
    return !getsockopt(socket, SOL_SOCKET, SO_PEERGROUPS, groups, &bytes) &&
           bytes == m->ngroups*sizeof(*groups) && !memcmp(groups, m->groups, bytes);
}

static struct watched_inode *find_inode(struct dcc_plan_monitor *m, int wd)
{
    struct watched_inode *inode;
    LIST_FOREACH(inode, &m->buckets[(unsigned)wd % 256], bucket)
        if (inode->wd == wd) return inode;
    return NULL;
}

static void drop_inode(struct dcc_plan_monitor *m, struct watched_inode *inode)
{
    LIST_REMOVE(inode, bucket);
    if (m->fd >= 0) inotify_rm_watch(m->fd, inode->wd);
    free(inode);
}

/* NULL selects inode events only. An empty child name selects the directory
 * listing. Any other child name selects that entry in its parent. */
static int subscribe(struct dcc_plan_watch *w, const char *path, const char *name, size_t length,
                     int content)
{
    uint32_t mask = IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_MASK_ADD;
    if (name) mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    if (content) mask |= IN_MODIFY | IN_CLOSE_WRITE;
    struct dcc_plan_monitor *m = w->monitor;
    int wd = inotify_add_watch(m->fd, path, mask);
    if (wd < 0) return 0;
    struct watched_inode *inode = find_inode(m, wd);
    if (!inode) {
        inode = calloc(1, sizeof(*inode));
        if (!inode) { inotify_rm_watch(m->fd, wd); return 0; }
        inode->wd = wd;
        LIST_INSERT_HEAD(&m->buckets[(unsigned)wd % 256], inode, bucket);
    }
    struct watch_interest *interest;
    LIST_FOREACH(interest, &w->paths, by_plan)
        if (interest->inode == inode && interest->children == !!name &&
            (!name || (strlen(interest->name) == length && !memcmp(interest->name, name, length)))) return 1;
    size_t bytes = sizeof(*interest) + length + 1;
    interest = malloc(bytes);
    if (!interest) {
        if (LIST_EMPTY(&inode->users)) drop_inode(m, inode);
        return 0;
    }
    interest->plan = w;
    interest->inode = inode;
    interest->children = !!name;
    if (name) memcpy(interest->name, name, length);
    interest->name[length] = 0;
    LIST_INSERT_HEAD(&inode->users, interest, by_inode);
    LIST_INSERT_HEAD(&w->paths, interest, by_plan);
    /* Charge a whole inode to this subscriber as an upper bound. */
    w->bytes += bytes + sizeof(*inode);
    return 1;
}

static int watch_path(struct dcc_plan_watch *w, const char *path, int listing, int content, unsigned links)
{
    char prefix[PATH_MAX] = "/";
    if (strlen(path) >= sizeof(prefix) || links > 40) return 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        size_t n = strcspn(p, "/"), offset = p - path;
        if (!subscribe(w, prefix, p, n, 0)) return 0;
        memcpy(prefix, path, offset+n);
        prefix[offset+n] = 0;
        char target[PATH_MAX];
        ssize_t size = readlink(prefix, target, sizeof(target));
        p += n;
        if (size < 0) {
            if (errno == EINVAL) continue;
            return errno == ENOENT || errno == ENOTDIR;
        }
        if (size == sizeof(target) || !subscribe(w, prefix, NULL, 0, 0)) return 0;
        target[size] = 0;
        char resolved[PATH_MAX];
        prefix[offset] = 0;
        int length = snprintf(resolved, sizeof(resolved), "%s%s%s", target[0] == '/' ? "" : prefix, target, p);
        return length >= 0 && length < (int)sizeof(resolved) && watch_path(w, resolved, listing, content, links+1);
    }
    return subscribe(w, prefix, listing ? "" : NULL, 0, content);
}
/* The broker only inspects dependencies. Never replay a client's open flags
 * or stdio mode in the broker. Unsupported results retain client validation. */
static int watched_record(const struct record *r)
{
    const struct dcc_plan_dependency *d = &r->d;
    if (d->operation != DCC_PLAN_OPEN && d->operation != DCC_PLAN_STREAM && d->operation != DCC_PLAN_DIRECTORY)
        return validate(d, r->path, r->target);
    struct stat st;
    if (stat(r->path, &st))
        return (errno == ENOENT || errno == ENOTDIR) && d->error == errno;
    return !d->error && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || !strcmp(r->path, "/dev/null")) &&
           !access(r->path, R_OK) && dcc_plan_stat_equal(d->operation, &st, &d->metadata);
}
void dcc_plan_watch_free(struct dcc_plan_watch *w)
{
    if (!w) return;
    while (!LIST_EMPTY(&w->paths)) {
        struct watch_interest *interest = LIST_FIRST(&w->paths);
        struct watched_inode *inode = interest->inode;
        LIST_REMOVE(interest, by_plan);
        LIST_REMOVE(interest, by_inode);
        if (LIST_EMPTY(&inode->users)) drop_inode(w->monitor, inode);
        free(interest);
    }
    free(w);
}

static void invalidate_all(struct dcc_plan_monitor *m)
{
    for (size_t i = 0; i < sizeof(m->buckets)/sizeof(*m->buckets); ++i) {
        struct watched_inode *inode;
        LIST_FOREACH(inode, &m->buckets[i], bucket) {
            struct watch_interest *interest;
            LIST_FOREACH(interest, &inode->users, by_inode) interest->plan->count = 0;
        }
    }
}

void dcc_plan_monitor_drain(struct dcc_plan_monitor *m)
{
    if (!m || m->fd < 0) return;
    union { struct inotify_event align; char bytes[8192]; } events;
    for (;;) {
        ssize_t n = read(m->fd, events.bytes, sizeof(events.bytes));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {
            invalidate_all(m);
            close(m->fd); m->fd = -1;
            return;
        }
        for (ssize_t at = 0; at < n;) {
            const struct inotify_event *e = (const void *)(events.bytes + at);
            at += sizeof(*e) + e->len;
            if (e->mask & IN_Q_OVERFLOW) { invalidate_all(m); continue; }
            struct watched_inode *inode = find_inode(m, e->wd);
            if (!inode) continue;
            struct watch_interest *interest;
            LIST_FOREACH(interest, &inode->users, by_inode)
                if (!e->len || (interest->children &&
                    (!*interest->name || !strcmp(interest->name, e->name))))
                    interest->plan->count = 0;
        }
    }
}

struct dcc_plan_watch *dcc_plan_watch_new(struct dcc_plan_monitor *m, const char *memo, size_t size,
                                          size_t *bytes)
{
    uint32_t lengths[2];
    if (!m || m->fd < 0 || size < 12) return NULL;
    memcpy(lengths, memo, 8);
    if ((uint64_t)lengths[0]+lengths[1]+8 != size || lengths[0] > DCC_PLAN_JOURNAL_LIMIT) return NULL;
    struct dcc_plan_watch *w = calloc(1, sizeof(*w));
    struct record *records = NULL;
    if (!w) return NULL;
    w->monitor = m;
    w->bytes = sizeof(*w);
    if (!parse_journal((const unsigned char *)memo+8, lengths[0], &records, &w->count)) goto failed;
    for (unsigned i = 0; i < w->count; ++i) {
        const struct record *r = &records[i];
        int listing = S_ISDIR(r->d.metadata.st_mode) && (r->d.operation == DCC_PLAN_DIRECTORY ||
            r->d.operation == DCC_PLAN_STREAM || r->d.operation == DCC_PLAN_OPEN);
        int content = !r->d.error && S_ISREG(r->d.metadata.st_mode);
        if (!watch_path(w, r->path, listing, content, 0) || !watched_record(r)) goto failed;
    }
    if (dcc_plan_watch_verified(w)) {
        *bytes = w->bytes;
        free(records);
        return w;
    }
failed:
    free(records);
    dcc_plan_watch_free(w);
    return NULL;
}

unsigned dcc_plan_watch_verified(struct dcc_plan_watch *w)
{
    if (!w || !w->count) return 0;
    dcc_plan_monitor_drain(w->monitor);
    return w->count;
}
#endif
