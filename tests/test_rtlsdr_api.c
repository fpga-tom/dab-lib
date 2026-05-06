#include "rtlsdr_api.h"

#include "test_support.h"

void test_rtlsdr_api_reports_unusable_when_not_loaded(void) {
    dab_rtlsdr_api_t api = {0};

    TEST_ASSERT(dab_rtlsdr_api_is_usable(&api) == 0);
}
