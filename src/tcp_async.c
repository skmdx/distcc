/* Parallel chunk processing; socket/job/cache state remains in the parent.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include "tcp_async.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static struct dcc_tcp_work *head, **tail = &head;
static struct dcc_tcp_work *completed, **completed_tail = &completed;
static unsigned outstanding; /* Parent-owned; includes uncollected completions. */
static pthread_t threads[2];
static unsigned count;
static int stopping, wake = -1;
static atomic_ulong tasks;
static atomic_uint active, peak;

static void *worker(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!head && !stopping) pthread_cond_wait(&ready, &lock);
        struct dcc_tcp_work *w = head;
        if (!w) { pthread_mutex_unlock(&lock); return NULL; }
        head = w->next;
        if (!head) tail = &head;
        pthread_mutex_unlock(&lock);
        unsigned running = atomic_fetch_add(&active, 1) + 1, observed = atomic_load(&peak);
        while (running > observed && !atomic_compare_exchange_weak(&peak, &observed, running)) {}
        w->run(w->argument);
        atomic_fetch_sub(&active, 1);
        atomic_fetch_add(&tasks, 1);
        pthread_mutex_lock(&lock);
        int notify = completed == NULL;
        w->next = NULL; *completed_tail = w; completed_tail = &w->next;
        if (notify) {
            uint64_t one = 1;
            while (write(wake, &one, sizeof(one)) < 0 && errno == EINTR) {}
        }
        pthread_mutex_unlock(&lock);
    }
}
int dcc_tcp_async_submit(struct dcc_tcp_work *w, void (*run)(void *),
                         void (*complete)(void *), void *argument)
{
    if (wake < 0 && (wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) return -1;
    if (!count) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        stopping = 0;
        unsigned limit = sysconf(_SC_NPROCESSORS_ONLN) > 1 ? 2 : 1;
        for (; count < limit; ++count)
            if (pthread_create(&threads[count], &attr, worker, NULL)) break;
        pthread_attr_destroy(&attr);
        if (!count) return -1;
    }
    w->run = run; w->complete = complete; w->argument = argument; w->next = NULL;
    ++outstanding;
    pthread_mutex_lock(&lock);
    *tail = w; tail = &w->next;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    return 0;
}
int dcc_tcp_async_fd(void) { return wake; }
int dcc_tcp_async_pending(void) { return outstanding != 0; }
unsigned long dcc_tcp_async_tasks(void) { return atomic_load(&tasks); }
unsigned dcc_tcp_async_peak(void) { return atomic_load(&peak); }
void dcc_tcp_async_drain(void)
{
    uint64_t value;
    pthread_mutex_lock(&lock);
    if (wake >= 0) while (read(wake, &value, sizeof(value)) < 0 && errno == EINTR) {}
    struct dcc_tcp_work *list = completed;
    completed = NULL; completed_tail = &completed;
    pthread_mutex_unlock(&lock);
    while (list) {
        struct dcc_tcp_work *w = list;
        list = w->next;
        void (*complete)(void *) = w->complete;
        void *argument = w->argument;
        w->next = NULL; w->run = NULL;
        --outstanding;
        complete(argument); /* May release or resubmit w. */
    }
}
void dcc_tcp_async_stop(void)
{
    pthread_mutex_lock(&lock);
    stopping = 1;
    pthread_cond_broadcast(&ready);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < count; ++i) pthread_join(threads[i], NULL);
    count = 0;
}
void dcc_tcp_async_child(void)
{
    if (wake >= 0) close(wake);
    wake = -1;
}
