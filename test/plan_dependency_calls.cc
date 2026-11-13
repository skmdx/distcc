/* Exercise the same public-libc observation boundary as the GCC preload. */
#define _GNU_SOURCE 1
#include "plan_memo.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" int __open_2(const char *,int);
extern "C" int __open64_2(const char *,int);
extern "C" int __openat_2(int,const char *,int);
extern "C" int __openat64_2(int,const char *,int);

int main(int argc,char **argv) {
    if (argc!=4) return 2;
    int journal=open(argv[1],O_CREAT|O_TRUNC|O_RDWR,0600);
    if (journal<0 || write(journal,"\0\0\0\0",4)!=4) return 2;
    const char *operation=argv[2],*path=argv[3];
    int prior=-1;
    if (strstr(operation,"empty")) prior=open(path,O_PATH|O_NOFOLLOW);
    dcc_plan_record_begin(journal,1);
    struct stat st{}; struct stat64 st64{}; int fd=-1,result=0;
    if (!strcmp(operation,"stat")) result=stat(path,&st);
    else if (!strcmp(operation,"lstat")) result=lstat(path,&st);
    else if (!strcmp(operation,"stat64")) result=stat64(path,&st64);
    else if (!strcmp(operation,"lstat64")) result=lstat64(path,&st64);
    else if (!strcmp(operation,"fstatat64")) result=fstatat64(AT_FDCWD,path,&st64,AT_SYMLINK_NOFOLLOW);
    else if (!strcmp(operation,"fstatat-empty")) result=fstatat(prior,"",&st,AT_EMPTY_PATH);
    else if (!strcmp(operation,"fstatat-null-empty")) result=fstatat(prior,nullptr,&st,AT_EMPTY_PATH);
    else if (!strcmp(operation,"fstatat64-empty")) result=fstatat64(prior,"",&st64,AT_EMPTY_PATH);
    else if (!strcmp(operation,"faccessat-empty")) result=faccessat(prior,"",F_OK,AT_EMPTY_PATH);
    else if (!strcmp(operation,"faccessat")) result=faccessat(AT_FDCWD,path,R_OK,AT_EACCESS);
    else if (!strcmp(operation,"eaccess")) result=eaccess(path,R_OK);
    else if (!strcmp(operation,"euidaccess")) result=euidaccess(path,R_OK);
    else if (!strcmp(operation,"__open_2")) fd=__open_2(path,O_RDONLY);
    else if (!strcmp(operation,"__open64_2")) fd=__open64_2(path,O_RDONLY);
    else if (!strcmp(operation,"__openat_2")) fd=__openat_2(AT_FDCWD,path,O_RDONLY);
    else if (!strcmp(operation,"__openat64_2")) fd=__openat64_2(AT_FDCWD,path,O_RDONLY);
    else if (!strcmp(operation,"openat64")) fd=openat64(AT_FDCWD,path,O_RDONLY);
    else if (!strcmp(operation,"opendir")) {
        DIR *dir=opendir(path);result=dir ? 0 : -1;if (dir) closedir(dir);
    } else if (!strcmp(operation,"readlinkat")) {
        char data[4096];result=readlinkat(AT_FDCWD,path,data,sizeof(data));
    } else if (!strcmp(operation,"freopen64")) {
        FILE *file=freopen64(path,"r",stdin); result=file ? 0 : -1;
    } else return 2;
    int error=errno;
    if (fd>=0) close(fd);
    dcc_plan_record_finish(0);
    dcc_plan_record_begin(journal,0);dcc_plan_record_finish(0);
    close(journal);if (prior>=0) close(prior);
    printf("%d %d\n",result,error);return 0;
}
