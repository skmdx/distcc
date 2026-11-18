/* Synchronous calls share a connection. Nested publishing and CPU leases have distinct tags. */
int dcc_session_begin(int credentials, unsigned *tag);
int dcc_session_fd(int lease);
void dcc_session_retire(int lease);
void dcc_session_finish(int lease);
void dcc_session_lost(void);
