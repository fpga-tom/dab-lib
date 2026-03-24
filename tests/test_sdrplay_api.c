#include "sdrplay_api.h"

#include "test_support.h"

void test_sdrplay_api_reports_unusable_when_not_loaded(void) {
    dab_sdrplay_api_t api = {0};

    TEST_ASSERT(dab_sdrplay_api_is_usable(&api) == 0);
}
