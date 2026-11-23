#pragma once
// Prepare once in the resident parent; install the policy in each execution child.
bool dcc_init_compiler_sandbox(bool client);
bool dcc_restrict_compiler_child();
