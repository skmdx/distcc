/* -*- c-file-style: "java"; indent-tabs-mode: nil -*-
 *
 * distcc -- A simple distributed compiler system
 * $Header: /data/cvs/distcc/src/h_exten.c,v 1.7 2003/07/13 08:08:02 mbp Exp $
 *
 * Copyright (C) 2002 by Martin Pool <mbp@samba.org>
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/**
 * Test harness for functions in compile.c. (Only one so far.)
 **/

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "compile.h"
#include "job.h"
#include "frontend_fs.h"
#include "native_worker.h"
#include "plan_memo.h"
#include "broker.h"
#include <inttypes.h>


#define USAGE \
"usage: h_compile COMMAND ARGS...\n" \
"where\n" \
"  COMMAND is dcc_fresh_dependency_exists,\n" \
"    with ARGS being DOTD_FNAME EXCL_PAT REF_TIME\n" \
"or\n" \
"  COMMAND is dcc_discrepancy_filename\n" \
"or dcc_wait_for_cpp [CHILD_EXIT_CODE]\n"

const char *rs_program_name = __FILE__;


int main(int argc, char *argv[])
{
    if (argc > 1 && (!strcmp(argv[1], "--gcc-plan") || !strcmp(argv[1], "--gcc-plan-capture")))
        return dcc_gcc_plan_execute(argv+2);
    rs_trace_set_level(RS_LOG_DEBUG);
    rs_add_logger(rs_logger_file, RS_LOG_DEBUG, NULL, STDERR_FILENO);
    if (argc < 2) {
        rs_log_error(USAGE);
        return 1;
    }

    if (!strcmp(argv[1], "plan_key") && argc > 4) {
        char *key=dcc_plan_key(argv+3,argv[2]);
        puts(key ? key : "bypass"); free(key);
    } else if ((!strcmp(argv[1], "plan") || !strcmp(argv[1], "plan_record")) && argc > 4) {
        struct dcc_job *job=NULL;
        int ret=dcc_gcc_frontend_prepare(argv+3,argv[2],&job);
        if (ret) return ret;
        /* Two NUL-delimited groups: exactly the live defaults and roots. */
        for (unsigned i=3, groups=0; !strcmp(argv[1],"plan") && job->argv[i] && groups<2; ++i) {
            fwrite(job->argv[i],1,strlen(job->argv[i])+1,stdout);
            if (!*job->argv[i]) ++groups;
        }
        dcc_job_free(job);
        if (!strcmp(argv[1],"plan_record")) {
            char *key=dcc_plan_key(argv+3,argv[2]),*memo=NULL;
            int owner=-1;
            if (key) owner=dcc_broker_memo_get(key,&memo);
            if (!memo) {
                if (owner>=0) close(owner);
                free(key); return 1;
            }
            puts(memo); free(memo); free(key);
        }
    } else if (!strcmp(argv[1],"plan_journal") && argc==3) {
        int journal=open(argv[2],O_RDONLY),output=dcc_job_memfd("test-plan");
        unsigned count=0;
        if (journal<0 || output<0 || write(output,"plan",4)!=4) return 1;
        char *memo=dcc_plan_memo_encode(journal,output,&count);
        if (memo) puts(memo);
        int valid=memo!=NULL;
        close(journal); close(output); free(memo); return valid ? 0 : 1;
    } else if ((!strcmp(argv[1],"plan_validate") || !strcmp(argv[1],"plan_dump")) && argc==3) {
        char text[65536]; FILE *file=fopen(argv[2],"r");
        if (!file || !fgets(text,sizeof(text),file)) return 1;
        fclose(file); text[strcspn(text,"\n")]=0;
        if (!strcmp(argv[1],"plan_validate")) {
            unsigned count=0; int output=dcc_job_memfd("test-plan");
            int valid=dcc_plan_memo_decode(text,output,&count);
            close(output); return valid ? 0 : 1;
        }
        /* Test-only journal inspection uses this host's public struct stat
         * ABI. Paths are length-delimited, so no pathname escaping is lost. */
        unsigned char data[32768]; size_t bytes=strlen(text)/2;
        for (size_t i=0;i<bytes;++i) {
            unsigned v; if (sscanf(text+2*i,"%2x",&v)!=1) return 1; data[i]=v;
        }
        if (bytes<12) return 1;
        uint32_t size; memcpy(&size,data,4);
        if (size+8>bytes) return 1;
        for (size_t at=12;at<8+size;) {
            struct dcc_plan_dependency d;
            if (at+sizeof(d)>8+size) return 1;
            memcpy(&d,data+at,sizeof(d)); at+=sizeof(d);
            if (at+d.length+d.target_length>8+size) return 1;
            printf("%u %u %d %u %u %ju %ju %o %u %u %jd %jd %jd\n",
                d.operation,d.flags,d.error,d.length,d.target_length,
                (uintmax_t)d.metadata.st_dev,(uintmax_t)d.metadata.st_ino,
                d.metadata.st_mode,d.metadata.st_uid,d.metadata.st_gid,
                (intmax_t)d.metadata.st_size,(intmax_t)d.metadata.st_mtim.tv_sec,
                (intmax_t)d.metadata.st_ctim.tv_sec);
            fwrite(data+at,1,d.length+d.target_length,stdout); putchar('\n');
            at+=d.length+d.target_length;
        }
    } else if (strcmp(argv[1], "dcc_fresh_dependency_exists") == 0) {
        if (argc != 5) {
            rs_log_error("dcc_fresh_dependency_exists expects DOTD_FNAME "
                         "EXCL_PAT REF_TIME");
            return 1;
        }
        errno = 0;
        char *ptr;
        time_t ref_time = (time_t)strtol(argv[4], &ptr, 0);
        if (errno || (*ptr != '\0')) {
            rs_log_error("strtol failed");
            return 1;
        } else {
            char *result;
            int ret;
            ret = dcc_fresh_dependency_exists((const char *)argv[2],
                                              (const char *)argv[3],
                                              ref_time,
                                              &result);
            if (ret)
                printf("h_compile.c: UNEXPECTED RETURN VALUE\n");
            else
                printf("result %s\n", result ? result : "(NULL)");
            if (result) free(result);
        }
    } else if (strcmp(argv[1], "dcc_discrepancy_filename") == 0) {
        if (argc != 2) {
            rs_log_error("dcc_discrepancy_filename expects no arguments");
            return 1;
        }
        char *result;
        int ret = dcc_discrepancy_filename(&result);
        if (ret)
            printf("h_compile.c: UNEXPECTED RETURN VALUE\n");
        else
            printf("%s", result ? result : "(NULL)");
    } else if (strcmp(argv[1], "dcc_wait_for_cpp") == 0) {
        pid_t pid = 0;
        int status = 0x1234;
        if (argc == 3) {
            pid = fork();
            if (pid < 0) return 1;
            if (!pid) _exit(atoi(argv[2]));
        }
        int ret = dcc_wait_for_cpp(pid, &status, "test.i");
        printf("%d %d\n", ret, status);
    } else {
        rs_log_error(USAGE);
        return 1;
    }
    return 0;
}
