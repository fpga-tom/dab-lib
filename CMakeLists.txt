cmake_minimum_required(VERSION 3.21)

project(dab_lib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DAB_BUILD_TESTS "Build unit tests" ON)
option(DAB_ENABLE_AVX2 "Enable AVX2 backend when supported" ON)
option(DAB_ENABLE_NEON "Enable NEON backend when supported" ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(dab
    src/core/block_pool.c
    src/core/frontend.c
    src/core/null_detect.c
    src/core/prs_acquire.c
    src/core/prs_fft.c
    src/core/prs_fft_scalar.c
    src/core/prs_score.c
    src/core/prs_template.c
    src/core/pc_queue.c
    src/core/receiver.c
    src/core/sync.c
    src/hal/sdr_device.c
    src/hal/sdrplay/sdrplay_api.c
    src/hal/sdrplay/sdrplay_adapter.c
    src/hal/sdrplay/sdrplay_adapter_real.c
    src/hal/sdrplay/sdrplay_adapter_stub.c
    src/hal/sdrplay/sdrplay_device.c
    src/os/os_cond.c
    src/os/os_mutex.c
    src/os/os_shared_lib.c
    src/os/os_thread.c
    src/os/os_time.c
    src/simd/simd.c
    src/simd/simd_scalar.c
)

target_include_directories(dab
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(dab_sdrplay_probe
    tools/sdrplay_probe.c
)

target_link_libraries(dab_sdrplay_probe PRIVATE dab)

add_executable(dab_sdrplay_null_probe
    tools/sdrplay_null_probe.c
)

target_link_libraries(dab_sdrplay_null_probe PRIVATE dab)

add_executable(dab_sdrplay_sync_probe
    tools/sdrplay_sync_probe.c
)

target_link_libraries(dab_sdrplay_sync_probe PRIVATE dab)

add_executable(dab_gen_mode_i_prs_table
    tools/gen_mode_i_prs_table.c
)

target_compile_definitions(dab
    PRIVATE
        $<$<BOOL:${DAB_ENABLE_AVX2}>:DAB_ENABLE_AVX2=1>
        $<$<BOOL:${DAB_ENABLE_NEON}>:DAB_ENABLE_NEON=1>
)

if(DAB_ENABLE_AVX2)
    if(MSVC)
        target_sources(dab PRIVATE src/simd/simd_avx2.c)
        target_sources(dab PRIVATE src/core/prs_fft_avx2.c)
        target_compile_options(dab PRIVATE /arch:AVX2)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
        target_sources(dab PRIVATE src/simd/simd_avx2.c)
        target_sources(dab PRIVATE src/core/prs_fft_avx2.c)
        target_compile_options(dab PRIVATE -mavx2)
    endif()
endif()

if(DAB_ENABLE_NEON)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|armv8)")
        target_sources(dab PRIVATE src/simd/simd_neon.c)
        target_sources(dab PRIVATE src/core/prs_fft_neon.c)
        if(NOT MSVC AND NOT APPLE)
            target_compile_options(dab PRIVATE -mfpu=neon)
        endif()
    endif()
endif()

if(DAB_BUILD_TESTS)
    enable_testing()

    add_executable(dab_tests
        tests/test_main.c
        tests/test_block_pool.c
        tests/test_frontend.c
        tests/test_hal.c
        tests/test_null_detect.c
        tests/test_os_shared_lib.c
        tests/test_sdrplay_api.c
        tests/test_sdrplay.c
        tests/test_pc_queue.c
        tests/test_simd.c
        tests/test_prs_acquire.c
        tests/test_prs_fft.c
        tests/test_prs_score.c
        tests/test_prs_template.c
        tests/test_sync.c
    )

    target_link_libraries(dab_tests PRIVATE dab)
    target_include_directories(dab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/sdrplay)

    add_test(NAME dab_tests COMMAND dab_tests)
endif()
