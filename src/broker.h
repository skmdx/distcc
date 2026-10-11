/* Local distccd scheduling and connected-socket leases. */
struct dcc_hostdef;
int dcc_broker_serve(const char *path);
int dcc_broker_acquire(struct dcc_hostdef **host, int *lease);
int dcc_broker_jobs(void);
