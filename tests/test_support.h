#ifndef DAB_TEST_SUPPORT_H
#define DAB_TEST_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>

#define TEST_ASSERT(EXPR) \
    do { \
        if (!(EXPR)) { \
            fprintf(stderr, "assertion failed: %s (%s:%d)\n", #EXPR, __FILE__, __LINE__); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#endif
