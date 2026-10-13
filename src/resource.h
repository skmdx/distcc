/* Host and cgroup resource admission, shared by clients and workers. */
double dcc_memory_pressure(void);
int dcc_resource_slots(int maximum);
void dcc_resource_observe(unsigned long long peak_bytes, int success);
unsigned long long dcc_resource_job_bytes(void);
unsigned dcc_resource_samples(void);
unsigned long long dcc_resource_peak_bytes(void);
