/* Classify a resolved ELF link for local admission. */
#pragma once

/* Zero for native inputs, byte count for a small bitcode link, -1 otherwise. */
int dcc_small_link_bytes(char **command);
