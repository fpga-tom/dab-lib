#include "dab/os/os_time.h"

#if defined(_WIN32)
#include <windows.h>

uint64_t dab_time_monotonic_ns(void) {
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t frequency;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    frequency = (uint64_t)freq.QuadPart;
    seconds = (uint64_t)now.QuadPart / frequency;
    remainder = (uint64_t)now.QuadPart % frequency;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / frequency;
}
#else
#include <time.h>

uint64_t dab_time_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}
#endif
