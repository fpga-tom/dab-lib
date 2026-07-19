#include <stdio.h>
#include <stdlib.h>

#include "dab/config.h"
#include "dab/os/os_time.h"
#include "dab/simd/simd.h"

int main(int argc, char **argv) {
    const dab_simd_ops_t *ops = dab_simd_get_ops();
    const uint32_t count = DAB_IQ_BLOCK_SAMPLES;
    uint32_t iterations = 20000u;
    int16_t *src;
    float *dst_i;
    float *dst_q;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    double samples_per_second;
    double gib_per_second;
    volatile float checksum;
    uint32_t i;

    if (argc > 1) {
        const unsigned long parsed = strtoul(argv[1], 0, 10);
        if (parsed == 0u || parsed > 1000000u) {
            fprintf(stderr, "iterations must be in range 1..1000000\n");
            return 2;
        }
        iterations = (uint32_t)parsed;
    }

    src = (int16_t *)malloc(sizeof(*src) * 2u * count);
    dst_i = (float *)malloc(sizeof(*dst_i) * count);
    dst_q = (float *)malloc(sizeof(*dst_q) * count);
    if (src == 0 || dst_i == 0 || dst_q == 0) {
        free(dst_q);
        free(dst_i);
        free(src);
        return 2;
    }
    for (i = 0u; i < 2u * count; ++i) {
        src[i] = (int16_t)((i * 4051u) & 0xFFFFu);
    }
    for (i = 0u; i < 100u; ++i) {
        ops->complex_i16_to_f32(dst_i, dst_q, src, count);
    }

    start_ns = dab_time_monotonic_ns();
    for (i = 0u; i < iterations; ++i) {
        ops->complex_i16_to_f32(dst_i, dst_q, src, count);
    }
    elapsed_ns = dab_time_monotonic_ns() - start_ns;
    checksum = dst_i[count - 1u] + dst_q[count - 1u];
    samples_per_second = ((double)iterations * (double)count * 1.0e9) / (double)elapsed_ns;
    gib_per_second = (samples_per_second * 12.0) / (1024.0 * 1024.0 * 1024.0);
    printf("backend=%s count=%u iterations=%u elapsed_ms=%.3f msamples_s=%.3f logical_gib_s=%.3f checksum=%.1f\n",
           ops->name,
           count,
           iterations,
           (double)elapsed_ns / 1.0e6,
           samples_per_second / 1.0e6,
           gib_per_second,
           (double)checksum);

    free(dst_q);
    free(dst_i);
    free(src);
    return 0;
}
