#include <stdio.h>
#include <stdlib.h>

void test_pc_queue_basic(void);
void test_pc_queue_stop_wakes_waiter(void);
void test_block_pool_acquire_release(void);
void test_frontend_converts_iq_and_preserves_metadata(void);
void test_frontend_dc_removal_reduces_constant_offset(void);
void test_hal_enumerates_sdrplay_stub(void);
void test_hal_opens_and_configures_sdrplay_stub(void);
void test_hal_rejects_unknown_driver(void);
void test_os_shared_lib_rejects_missing_library(void);
void test_sdrplay_api_reports_unusable_when_not_loaded(void);
void test_sdrplay_device_specific_config_propagates_to_samples(void);
void test_sdrplay_rejects_invalid_device_specific_config(void);
void test_simd_scalar_fallback(void);
void test_simd_convert_and_dot(void);

int main(void) {
    test_pc_queue_basic();
    test_pc_queue_stop_wakes_waiter();
    test_block_pool_acquire_release();
    test_frontend_converts_iq_and_preserves_metadata();
    test_frontend_dc_removal_reduces_constant_offset();
    test_hal_enumerates_sdrplay_stub();
    test_hal_opens_and_configures_sdrplay_stub();
    test_hal_rejects_unknown_driver();
    test_os_shared_lib_rejects_missing_library();
    test_sdrplay_api_reports_unusable_when_not_loaded();
    test_sdrplay_device_specific_config_propagates_to_samples();
    test_sdrplay_rejects_invalid_device_specific_config();
    test_simd_scalar_fallback();
    test_simd_convert_and_dot();
    puts("all tests passed");
    return EXIT_SUCCESS;
}
