/* Local distccd scheduling and connected-socket leases. */
struct dcc_hostdef;
struct dcc_work_hint;
struct dcc_job_result;
int dcc_broker_serve(const char *path);
int dcc_broker_acquire(struct dcc_hostdef **host, int *lease, int cpp_ready, const struct dcc_work_hint *);
int dcc_broker_local_cpp(int *lease, int *occupied, int analysis);
void dcc_broker_failed(const struct dcc_hostdef *host);
void dcc_broker_defer(const struct dcc_hostdef *host);
void dcc_broker_return(const struct dcc_hostdef *host, int fd, const struct dcc_job_result *);
#define DCC_BROKER_POOLED (1 << 30)
#define DCC_TCP_PIPELINE 4
int dcc_broker_jobs(void);
void dcc_broker_cpp_done(const struct dcc_hostdef *host);

/* A cache miss returns a publishing lease; closing it abandons the probe.
 * Concurrent readers wait for that lease, never for work in the broker. */
#define DCC_MEMO_LIMIT 65536
int dcc_broker_memo_get(const char *key, char **value);
void dcc_broker_memo_put(int lease, const char *value);
