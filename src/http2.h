/* HTTP/2 sessions owned by the broker / prefork event loop. */
#ifndef DCC_HTTP2_H
#define DCC_HTTP2_H
#include <poll.h>
#define DCC_H2_PIPELINE 4

struct dcc_h2;
/* Takes fd on success. A refusal must not start a job or retain fd. */
typedef int (*dcc_h2_accept_fn)(void *context, int fd, int link);
struct dcc_h2 *dcc_h2_new(int fd, dcc_h2_accept_fn accept_job, void *context);
void dcc_h2_free(struct dcc_h2 *connection);
/* The returned socket carries one RPC. status is updated until completion:
 * 0 pending, 200 admitted, 503 refused without execution, -1 transport error.
 * Its storage must remain alive until cancel() or session destruction. */
int dcc_h2_open(struct dcc_h2 *connection, int link, int *fd, int *status, int *file_cache);
void dcc_h2_cancel(struct dcc_h2 *connection, int stream);
int dcc_h2_prepare(struct dcc_h2 *connection, struct pollfd *polls, int capacity);
int dcc_h2_process(struct dcc_h2 *connection, struct pollfd *polls);
int dcc_h2_idle(struct dcc_h2 *connection);
int dcc_h2_active(struct dcc_h2 *connection);
int dcc_h2_staged(struct dcc_h2 *connection);
#endif
