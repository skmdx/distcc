/* Exercise state ownership through the real writer and monitor APIs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "distcc.h"

const char *rs_program_name = "h_state";

int main(void)
{
    int command, status;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    assert(!atexit(dcc_remove_state_file));
    assert(!dcc_note_state(DCC_PHASE_COMPILE, "owner.c", "worker", DCC_REMOTE));
    printf("%ld\n", (long)getpid());
    while ((command = getchar()) != EOF) {
        if (command == 'p') {
            assert(!dcc_note_state(DCC_PHASE_RECEIVE, NULL, NULL, DCC_REMOTE));
        } else if (command == 'f' || command == 'c') {
            pid_t child = fork();
            assert(child >= 0);
            if (!child) {
                if (command == 'c')
                    assert(!dcc_note_state(DCC_PHASE_CPP, "child.c", "localhost", DCC_LOCAL));
                dcc_remove_state_file();
                exit(0);
            }
            assert(waitpid(child, &status, 0) == child && status == 0);
        } else if (command == 'h') {
            pid_t child = fork();
            assert(child >= 0);
            if (!child) {
                for (;;) pause();
            }
            printf("%ld\n", (long)child);
            continue;
        } else if (command == 'e') {
            execl("/bin/cat", "cat", (char *)NULL);
            abort();
        } else if (command == 'q') {
            return 0;
        } else {
            continue;
        }
        puts("ok");
    }
    return 0;
}
