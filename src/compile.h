/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
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

/* remote.c */

#include <sys/types.h>
#include <stdint.h>
struct dcc_hostdef;
int dcc_wait_for_cpp(pid_t cpp_pid, int *status, const char *input_fname);

struct dcc_buffer;
struct dcc_job;
int dcc_compile_remote(char **argv,
                       char *input_fname,
                       char *cpp_fname,
                       int cpp_fd,
                       char **auxiliary,
                       struct dcc_job *prepared_job,
                       char *output_fname,
                       char *deps_fname,
                       struct dcc_buffer *server_stderr,
                       struct dcc_buffer *server_stdout,
                       pid_t cpp_pid,
                       int local_cpu_lock_fd,
                       struct dcc_hostdef *host,
                       int *status,
                       int *unsupported);

/* compile.c */


int dcc_run_local_command(char **argv);
int dcc_link_dispatch(char **argv);
int dcc_compile_local(char **argv, char *input, const char *prepared, uint64_t *cpu_ns);

int dcc_build_somewhere_timed(char *argv[],
                              int sg_level,
                              int *status);
