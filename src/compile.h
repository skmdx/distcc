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
#include <time.h>
struct dcc_hostdef;
int dcc_wait_for_cpp(pid_t cpp_pid, int *status, const char *input_fname);

struct dcc_buffer;
struct dcc_job;
int dcc_compile_remote(char **argv,
                       char *input_fname,
                       char *cpp_fname,
                       int cpp_fd,
                       char **file_names,
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

extern int dcc_scan_includes;

int dcc_build_somewhere_timed(char *argv[],
                              int sg_level,
                              int *status);

/* Declared here for testing purposes. */
int dcc_fresh_dependency_exists(const char *dotd_fname,
                                const char *exlude_pat,
                                time_t reference_time,
                                char **fresh_dependency);

int dcc_discrepancy_filename(char **filename);
