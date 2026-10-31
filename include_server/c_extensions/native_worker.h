/* The helper owns its control socket and all invocation children. */
#pragma once
int dcc_native_worker(int control, int (*execute)(int command, bool preparing));
