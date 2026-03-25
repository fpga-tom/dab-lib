#include "dab/core/tframe.h"

#include "test_support.h"

void test_tframe_push_symbol_partitions_fic_and_msc_and_wraps_frame(void) {
    dab_tframe_t tframe;
    dab_tframe_buffers_t buffers;
    dab_tframe_result_t result;
    int8_t fic[DAB_TFRAME_FIC_SOFTBITS] = {0};
    int8_t msc[DAB_TFRAME_MSC_SOFTBITS] = {0};
    int8_t frame[DAB_TFRAME_TOTAL_SOFTBITS] = {0};
    int8_t soft_a[1536];
    int8_t soft_b[1536];
    uint32_t i;
    uint32_t sym;

    buffers.fic_softbits = fic;
    buffers.msc_softbits = msc;
    buffers.frame_softbits = frame;
    TEST_ASSERT(dab_tframe_init(&tframe, &buffers) == 0);

    for (i = 0u; i < 1536u; ++i) {
        soft_a[i] = 1;
        soft_b[i] = -1;
    }

    for (sym = 0u; sym < DAB_TFRAME_TOTAL_SYMBOLS; ++sym) {
        soft_a[0] = (int8_t)sym;
        soft_b[0] = (int8_t)(-((int)sym));
        TEST_ASSERT(dab_tframe_push_symbol(&tframe, 1000u + sym, soft_a, soft_b, &result) == 0);
    }

    TEST_ASSERT(fic[0] == 0);
    TEST_ASSERT(fic[1] == 0);
    TEST_ASSERT(fic[2u * DAB_TFRAME_SOFTBITS_PER_SYMBOL] == 2);
    TEST_ASSERT(msc[0] == 3);
    TEST_ASSERT(msc[1] == -3);
    TEST_ASSERT(result.frame_complete == 1);
    TEST_ASSERT(tframe.symbol_index == 0u);
}
