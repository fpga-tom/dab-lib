#include "dab/os/os_time.h"

#if defined(_WIN32)
#include <windows.h>

uint64_t dab_time_monotonic_ns(void) {
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((1000000000ull * (uint64_t)now.QuadPart) / (uint64_t)freq.QuadPart);
}
#else
#include <time.h>

uint64_t dab_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}
#endif
