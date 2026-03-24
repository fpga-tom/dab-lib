#ifndef DAB_TYPES_H
#define DAB_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#define DAB_ALIGN(N) __declspec(align(N))
#else
#define DAB_ALIGN(N) __attribute__((aligned(N)))
#endif

#endif
