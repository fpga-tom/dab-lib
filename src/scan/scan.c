#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#define DAB_SCAN_NO_MAIN
#include "../../tools/dab_sdrplay_scan.c"
