#include "dab/os/os_shared_lib.h"

#include "test_support.h"

void test_os_shared_lib_rejects_missing_library(void) {
    dab_shared_lib_t lib;

    TEST_ASSERT(dab_shared_lib_open(&lib, "definitely_missing_codex_test_library") != 0);
}
