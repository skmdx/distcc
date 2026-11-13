/* Memoized GCC plans are validated by the requesting sandboxed client. */
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
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

char *dcc_plan_key(char **argv, const char *input)
{
    const char *library = getenv("DISTCC_GCC_PLAN_LIBRARY");
    static const char *const omitted[] = {"DISTCC_LOG", "DISTCC_VERBOSE", NULL};
    char context[80], *compiler = NULL, cwd[PATH_MAX];
    const char *extension = dcc_find_extension_const(input), *parent = strrchr(input, '/');
    if (!getenv("DISTCC_BROKER") || !extension || !strcmp(input, "-") ||
        !getcwd(cwd, sizeof(cwd))) return NULL;
    if (!library) library = GCC_PLAN_LIBRARY;
    if (strchr(argv[0], '/')) compiler = strdup(argv[0]);
    else if (dcc_which(argv[0], &compiler)) return NULL;
    if (!compiler) return NULL;
    int failed = dcc_local_worker_key(compiler, library, omitted, context, sizeof(context));
    free(compiler);
    if (failed) return NULL;
    XXH3_state_t hash;
    XXH3_128bits_reset(&hash);
#define ADD(s) XXH3_128bits_update(&hash, (s), strlen(s)+1)
    ADD("gcc-plan-1"); ADD(context); ADD(cwd); ADD(extension);
    if (parent) XXH3_128bits_update(&hash, input, parent-input+1);
    else ADD(".");
    unsigned inputs = 0, compile = 0;
    size_t bytes = 0;
    for (unsigned i=0; argv[i]; ++i) {
        const char *s = argv[i];
        bytes += strlen(s)+1;
        if (bytes > DCC_MEMO_LIMIT || *s=='@' || !strcmp(s,"-") ||
            !strcmp(s,"-E") || !strcmp(s,"-v") || !strcmp(s,"-###") ||
            !strncmp(s,"-specs",6) || !strncmp(s,"--specs",7) ||
            !strncmp(s,"-wrapper",8) || !strncmp(s,"-fplugin",8) ||
            !strncmp(s,"--config",8)) return NULL;
        if (!strcmp(s,"-c")) compile = 1;
        if (!strcmp(s,"-o") || !strcmp(s,"-MF") || !strcmp(s,"-MT") || !strcmp(s,"-MQ")) {
            ADD(s);
            if (!argv[++i]) return NULL;
            bytes += strlen(argv[i])+1;
            continue;
        }
        if (!strcmp(s,input)) { ++inputs; ADD("<input>"); }
        else {
            /* The caller already owns single-input option validation. Reject
             * a second source operand here rather than interpreting options. */
            if (i && *s!='-' && dcc_is_source(s)) return NULL;
            ADD(s);
        }
    }
    if (!compile || inputs!=1 || bytes>DCC_MEMO_LIMIT) return NULL;
    XXH128_hash_t value=XXH3_128bits_digest(&hash);
    char *key=NULL;
    if (asprintf(&key,"gcc-plan:%016llx%016llx",(unsigned long long)value.high64,
                 (unsigned long long)value.low64)<0) return NULL;
    return key;
#undef ADD
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
static int journal_records(const unsigned char *data, size_t length,
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

static unsigned char *file_bytes(int fd, size_t *size)
{
    struct stat st;
    if (fstat(fd,&st) || st.st_size<0 || st.st_size>DCC_PLAN_JOURNAL_LIMIT) return NULL;
    unsigned char *data=malloc(st.st_size+1);
    if (!data) return NULL;
    if (pread(fd,data,st.st_size,0)!=st.st_size) { free(data); return NULL; }
    *size=st.st_size; return data;
}

char *dcc_plan_memo_encode(int journal, int output, unsigned *dependencies)
{
    size_t size=0,plan_size=0;
    unsigned char *data=file_bytes(journal,&size), *plan=file_bytes(output,&plan_size);
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
    if (2*(8+compact+plan_size)+1>DCC_MEMO_LIMIT) goto out;
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
    memo=malloc(at*2+1);
    if (memo) {
        const char hex[]="0123456789abcdef";
        for (size_t i=0;i<at;++i) { memo[2*i]=hex[packed[i]>>4]; memo[2*i+1]=hex[packed[i]&15]; }
        memo[2*at]=0;
    }
    free(packed);
out:
    free(records); free(data); free(plan); return memo;
}

int dcc_plan_memo_decode(const char *memo, int output, unsigned *dependencies)
{
    size_t size=strlen(memo);
    if (size<24 || size>=DCC_MEMO_LIMIT || size%2) return 0;
    unsigned char *data=malloc(size/2);
    if (!data) return 0;
    int valid=0;
    struct record *records=NULL;
    for (size_t i=0;i<size;i+=2) {
        const char *a=strchr("0123456789abcdef",memo[i]),*b=strchr("0123456789abcdef",memo[i+1]);
        if (!a || !b) goto out;
        data[i/2]=(a-"0123456789abcdef")*16+(b-"0123456789abcdef");
    }
    uint32_t lengths[2]; memcpy(lengths,data,8);
    if ((uint64_t)lengths[0]+lengths[1]+8!=size/2 || lengths[0]>DCC_PLAN_JOURNAL_LIMIT ||
        !journal_records(data+8,lengths[0],&records,dependencies)) goto out;
    if (ftruncate(output,0) || lseek(output,0,SEEK_SET)<0 ||
        write(output,data+8+lengths[0],lengths[1])!=(ssize_t)lengths[1]) goto out;
    valid=1;
out:
    free(records); free(data); return valid;
}
