/* Host and cgroup resource admission, shared by clients and workers. */
#ifndef DCC_RESOURCE_H
#define DCC_RESOURCE_H
#define DCC_MEMORY_BUCKETS 64
double dcc_memory_pressure(void);
int dcc_resource_slots(int maximum);
int dcc_resource_cpus(void);
int dcc_resource_analysis_slots(int maximum);
struct dcc_job;
unsigned dcc_resource_bucket(unsigned long long bytes);
unsigned dcc_resource_class(const struct dcc_job *);
unsigned long long dcc_resource_input_bytes(const struct dcc_job *);
int dcc_resource_known(unsigned memory_class);
unsigned long long dcc_resource_estimate(unsigned memory_class);
/* Additional slots after existing reservations. */
int dcc_resource_class_slots(int maximum, unsigned memory_class,
                             unsigned long long reserved);
int dcc_resource_admit(unsigned memory_class, unsigned long long reserved);
void dcc_resource_observe(unsigned memory_class, unsigned long long peak_bytes, int success);
unsigned long long dcc_resource_job_bytes(void);
unsigned dcc_resource_samples(void);
unsigned long long dcc_resource_peak_bytes(void);
unsigned long long dcc_resource_spare_bytes(void);
#endif
