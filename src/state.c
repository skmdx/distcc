/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2003 by Martin Pool <mbp@samba.org>
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

#include <config.h>

#include <sys/random.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "types.h"
#include "distcc.h"
#include "trace.h"
#include "exitcode.h"
#include "snprintf.h"
#include "util.h"

const char *dcc_state_prefix = "binstate_";


static struct dcc_task_state *my_state = NULL;
static struct dcc_task_state local_state, remote_state;
static int state_fd = -1;
static pid_t state_owner;
static char *state_path;

static struct dcc_task_state *direct_my_state(const enum dcc_host target);

/**
 * @file
 *
 * This file provides a way for distcc processes to make little notes
 * about what they're up to that can be read by a monitor process.
 *
 * State is stored as follows.
 *
 * Within our temporary directory, we create a subdirectory called "state".
 *
 * Each process holds one uniquely named file open and overwrites its record.
 * A process-associated write lock denotes a live owner, including across PID
 * namespaces. The initial record is published only after taking that lock.
 *
 * Inside each of these, we store a binary struct in the native host
 * encoding.  Ugly, but quick and easy both in code and CPU time.
 *
 * Any process reading these files needs to handle the fact that they may be
 * truncated or otherwise incorrect.
 *
 * When the process exits, it removes its state file.  If you didn't
 * notice it already, it's too late now.
 *
 * An unlocked file is orphaned and can be removed by a monitor. Record locks
 * are not inherited by fork; children discard the inherited descriptor before
 * making their own record. CLOEXEC prevents leaking it into compilers.
 *
 * The reader interface for these files is in mon.c
 *
 * These files are considered a private format, and they may change
 * between distcc releases.  The only supported way to read them is
 * through mon.c.
 **/


const char *dcc_get_phase_name(enum dcc_phase phase)
{
    switch (phase) {
    case DCC_PHASE_STARTUP:
        return "Startup";
    case DCC_PHASE_BLOCKED:
        return "Blocked";
    case DCC_PHASE_COMPILE:
        return "Compile";
    case DCC_PHASE_CPP:
        return "Preprocess";
    case DCC_PHASE_CONNECT:
        return "Connect";
    case DCC_PHASE_SEND:
        return "Send";
    case DCC_PHASE_RECEIVE:
        return "Receive";
    case DCC_PHASE_DONE:
        return "Done";
    default:
        return "Unknown";
    }
}


/**
 * Remove the state file for this process.
 *
 * This can be called from atexit().
 **/
void dcc_remove_state_file (void)
{
    if (state_path && state_owner == getpid() &&
        unlink(state_path) && errno != ENOENT)
        rs_log_warning("failed to unlink %s: %s", state_path, strerror(errno));
    if (state_fd >= 0)
        dcc_close(state_fd);
    free(state_path);
    state_path = NULL;
    state_fd = -1;
    state_owner = 0;
}


static int dcc_write_state(int fd)
{
    ssize_t written;
    do {
        written = pwrite(fd, my_state, sizeof *my_state, 0);
    } while (written < 0 && errno == EINTR);
    if (written == sizeof *my_state)
        return 0;
    rs_log_warning("failed to write state: %s",
                   written < 0 ? strerror(errno) : "short write");
    return EXIT_IO_ERROR;
}

static int dcc_publish_state(void)
{
    char *dir, *hidden = NULL;
    unsigned long long id;
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    int ret = dcc_get_state_dir(&dir);
    ssize_t n;

    if (ret) return ret;
    do {
        n = getrandom(&id, sizeof id, 0);
    } while (n < 0 && errno == EINTR);
    if (n != sizeof id) return EXIT_IO_ERROR;
    if (asprintf(&state_path, "%s/%s%ld-%016llx", dir, dcc_state_prefix,
                 (long)getpid(), id) < 0)
        return EXIT_OUT_OF_MEMORY;
    if (asprintf(&hidden, "%s/.%s%ld-%016llx", dir, dcc_state_prefix,
                 (long)getpid(), id) < 0) {
        free(state_path);
        state_path = NULL;
        return EXIT_OUT_OF_MEMORY;
    }
    state_owner = getpid();
    /* Keep the existing umask/default ACL behavior, including group readers. */
    state_fd = open(hidden, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
    if (state_fd < 0) {
        rs_log_warning("failed to create state: %s", strerror(errno));
        ret = EXIT_IO_ERROR;
    } else {
        ret = fcntl(state_fd, F_SETLK, &lock) < 0 ? EXIT_IO_ERROR :
              dcc_write_state(state_fd);
        if (!ret && link(hidden, state_path)) ret = EXIT_IO_ERROR;
        unlink(hidden);
    }
    free(hidden);
    if (ret) {
        free(state_path);
        state_path = NULL;
        dcc_remove_state_file();
    }
    return ret;
}


/**
 * Record the state of this process.
 *
 * The filename is trimmed down to its basename.
 *
 * If the source_file or host are NULL, then are left unchanged from
 * their previous value.
 **/
int dcc_note_state(enum dcc_phase state,
                   const char *source_file,
                   const char *host, enum dcc_host target)
{
    int ret;


	if (!direct_my_state(target))
		return -1;

    my_state->struct_size = sizeof *my_state;
    my_state->magic = DCC_STATE_MAGIC;
    my_state->cpid = (unsigned long) getpid();

    source_file = dcc_find_basename(source_file);
    if (source_file) {
        strlcpy(my_state->file, source_file, sizeof my_state->file);
    }

    if (host) {
        strlcpy(my_state->host, host, sizeof my_state->host);
    }

    my_state->curr_phase = state;

    rs_trace("note state %d, file \"%s\", host \"%s\"",
             state,
             source_file ? source_file : "(NULL)",
             host ? host : "(NULL)");

    if (state_fd >= 0 && state_owner != getpid())
        dcc_remove_state_file();
    if (state_fd < 0)
        return dcc_publish_state();
    ret = dcc_write_state(state_fd);
    if (ret) dcc_remove_state_file();
    return ret;
}


void dcc_note_state_slot(int slot, enum dcc_host target)
{
	if (direct_my_state(target))
		my_state->slot = slot;
}


/**
	Point 'my_state' to the local or remote host state information, depending on target.

	Return 'my_state' pointer.
**/
static struct dcc_task_state *direct_my_state(const enum dcc_host target)
{
	switch (target)
	{
		case DCC_LOCAL:
			my_state = &local_state;
			break;

		case DCC_REMOTE:
			my_state = &remote_state;
			break;

		case DCC_UNKNOWN:
			break;
	}

	if (!my_state)
		rs_log_error("my_state == NULL");

	return my_state;
}
