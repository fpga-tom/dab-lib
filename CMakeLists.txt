cmake_minimum_required(VERSION 3.21)

project(dab_lib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DAB_BUILD_TESTS "Build unit tests" ON)
option(DAB_ENABLE_AVX2 "Enable AVX2 backend when supported" ON)
option(DAB_ENABLE_NEON "Enable NEON backend when supported" ON)

find_package(PkgConfig QUIET)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(dab
    src/core/block_pool.c
    src/core/frontend.c
    src/core/fic.c
    src/core/msc.c
    src/core/null_detect.c
    src/core/ofdm.c
    src/core/prs_acquire.c
    src/core/prs_fft.c
    src/core/prs_fft_scalar.c
    src/core/prs_score.c
    src/core/prs_template.c
    src/core/pc_queue.c
    src/core/receiver.c
    src/core/sync.c
    src/core/tframe.c
    src/core/viterbi47_port.c
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

add_executable(dab_sdrplay_audio
    tools/dab_sdrplay_audio.c
    tools/audio_playback.c
)

target_link_libraries(dab_sdrplay_audio PRIVATE dab)

add_executable(dab_sync_file_replay
    tools/dab_sync_file_replay.c
    tools/audio_playback.c
)

target_link_libraries(dab_sync_file_replay PRIVATE dab)
if(PkgConfig_FOUND)
    pkg_check_modules(FDK_AAC IMPORTED_TARGET QUIET fdk-aac)
    pkg_check_modules(FAAD2 IMPORTED_TARGET QUIET faad2)
    pkg_check_modules(PORTAUDIO IMPORTED_TARGET QUIET portaudio-2.0)
    if(PORTAUDIO_FOUND AND (FDK_AAC_FOUND OR FAAD2_FOUND))
        target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
        target_link_libraries(dab_sync_file_replay PRIVATE PkgConfig::PORTAUDIO)
        if(FDK_AAC_FOUND)
            target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_FDK_AAC=1)
            target_link_libraries(dab_sync_file_replay PRIVATE PkgConfig::FDK_AAC)
        endif()
        if(FAAD2_FOUND)
            target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_FAAD2=1)
            target_link_libraries(dab_sync_file_replay PRIVATE PkgConfig::FAAD2)
        endif()

        target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
        target_link_libraries(dab_sdrplay_audio PRIVATE PkgConfig::PORTAUDIO)
        if(FDK_AAC_FOUND)
            target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_FDK_AAC=1)
            target_link_libraries(dab_sdrplay_audio PRIVATE PkgConfig::FDK_AAC)
        endif()
        if(FAAD2_FOUND)
            target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_FAAD2=1)
            target_link_libraries(dab_sdrplay_audio PRIVATE PkgConfig::FAAD2)
        endif()
    endif()
endif()

set(DAB_REF_IMPL_AUDIO_SOURCE ref-impl/dabplus/audio_stub.c)
set(DAB_REF_IMPL_AUDIO_LIBS "")
set(DAB_REF_IMPL_AUDIO_DEFINES "")
if(PkgConfig_FOUND)
    pkg_check_modules(FAAD2 IMPORTED_TARGET QUIET faad2)
    if(PORTAUDIO_FOUND AND FAAD2_FOUND)
        set(DAB_REF_IMPL_AUDIO_SOURCE ref-impl/dabplus/paudio.c)
        set(DAB_REF_IMPL_AUDIO_LIBS PkgConfig::PORTAUDIO PkgConfig::FAAD2)
        set(DAB_REF_IMPL_AUDIO_DEFINES PORTAUDIO=1)
    endif()
endif()

add_executable(dab_gen_mode_i_prs_table
    tools/gen_mode_i_prs_table.c
)

add_executable(dab_fic_dump_analyze
    tools/dab_fic_dump_analyze.c
)

target_link_libraries(dab_fic_dump_analyze PRIVATE dab)

add_executable(dab_fic_symbol_dump_analyze
    tools/dab_fic_symbol_dump_analyze.c
)

target_link_libraries(dab_fic_symbol_dump_analyze PRIVATE dab)

add_executable(dab_fic_carrier_dump_analyze
    tools/dab_fic_carrier_dump_analyze.c
)

target_link_libraries(dab_fic_carrier_dump_analyze PRIVATE dab)

add_executable(dab_fic_diff_dump_analyze
    tools/dab_fic_diff_dump_analyze.c
)

target_link_libraries(dab_fic_diff_dump_analyze PRIVATE dab)

add_executable(dab_fic_active_dump_analyze
    tools/dab_fic_active_dump_analyze.c
)

target_link_libraries(dab_fic_active_dump_analyze PRIVATE dab)

add_executable(dab_fic_raw_active_dump_analyze
    tools/dab_fic_raw_active_dump_analyze.c
)

target_link_libraries(dab_fic_raw_active_dump_analyze PRIVATE dab)
target_include_directories(dab_fic_raw_active_dump_analyze PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_executable(dab_fic_block_dump_analyze
    tools/dab_fic_block_dump_analyze.c
)

target_link_libraries(dab_fic_block_dump_analyze PRIVATE dab)

add_executable(dab_fic_input_compare
    tools/dab_fic_input_compare.c
)

target_link_libraries(dab_fic_input_compare PRIVATE dab)

add_executable(dab_fic_unpuncture_compare
    tools/dab_fic_unpuncture_compare.c
)

target_link_libraries(dab_fic_unpuncture_compare PRIVATE dab)

add_executable(dab_fic_postviterbi_compare
    tools/dab_fic_postviterbi_compare.c
)

target_link_libraries(dab_fic_postviterbi_compare PRIVATE dab)

add_executable(dab_prs_active_compare
    tools/dab_prs_active_compare.c
)

target_include_directories(dab_prs_active_compare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_executable(dab_prs_fft_compare
    tools/dab_prs_fft_compare.c
)

target_link_libraries(dab_prs_fft_compare PRIVATE dab)
target_include_directories(dab_prs_fft_compare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_executable(dab_prs_ref_compare
    tools/dab_prs_ref_compare.c
)

target_link_libraries(dab_prs_ref_compare PRIVATE dab)

add_executable(dab_ofdm_chain_compare
    tools/dab_ofdm_chain_compare.c
)

target_link_libraries(dab_ofdm_chain_compare PRIVATE dab)
target_include_directories(dab_ofdm_chain_compare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

add_executable(dab_fic_ref_compare
    tools/dab_fic_ref_compare.c
)

target_link_libraries(dab_fic_ref_compare PRIVATE dab)

add_executable(dab_fic_qpsk_compare
    tools/dab_fic_qpsk_compare.c
)

target_link_libraries(dab_fic_qpsk_compare PRIVATE dab)

add_executable(dab_ref_impl_replay
    tools/ref_impl_replay.c
    tools/ref_impl_backend_stubs.c
    ref-impl/dabplus/approx.c
    ${DAB_REF_IMPL_AUDIO_SOURCE}
    ref-impl/dabplus/backend_defaults.c
    ref-impl/dabplus/cagc.c
    ref-impl/dabplus/cc.c
    ref-impl/dabplus/ci.c
    ref-impl/dabplus/dab.c
    ref-impl/dabplus/dab_fic.c
    ref-impl/dabplus/fec.c
    ref-impl/dabplus/fi.c
    ref-impl/dabplus/fib.c
    ref-impl/dabplus/fs.c
    ref-impl/dabplus/gf.c
    ref-impl/dabplus/mod.c
    ref-impl/dabplus/ofdm.c
    ref-impl/dabplus/pilot.c
    ref-impl/dabplus/rs_255_245_5.c
    ref-impl/dabplus/sdrplay_api_stub.c
    ref-impl/dabplus/softbit_fft.c
    ref-impl/dabplus/util.c
    ref-impl/dabplus/viterbi47_port.c
)
target_link_libraries(dab_ref_impl_replay PRIVATE ${DAB_REF_IMPL_AUDIO_LIBS})
target_compile_definitions(dab_ref_impl_replay PRIVATE ${DAB_REF_IMPL_AUDIO_DEFINES})

target_include_directories(dab_ref_impl_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ref-impl/dabplus
)

add_executable(dab_ref_symbol_dump
    tools/dab_ref_symbol_dump.c
    tools/ref_impl_backend_stubs.c
    ref-impl/dabplus/approx.c
    ${DAB_REF_IMPL_AUDIO_SOURCE}
    ref-impl/dabplus/backend_defaults.c
    ref-impl/dabplus/cagc.c
    ref-impl/dabplus/cc.c
    ref-impl/dabplus/ci.c
    ref-impl/dabplus/dab.c
    ref-impl/dabplus/dab_fic.c
    ref-impl/dabplus/fec.c
    ref-impl/dabplus/fi.c
    ref-impl/dabplus/fib.c
    ref-impl/dabplus/fs.c
    ref-impl/dabplus/gf.c
    ref-impl/dabplus/mod.c
    ref-impl/dabplus/ofdm.c
    ref-impl/dabplus/pilot.c
    ref-impl/dabplus/rs_255_245_5.c
    ref-impl/dabplus/sdrplay_api_stub.c
    ref-impl/dabplus/softbit_fft.c
    ref-impl/dabplus/util.c
    ref-impl/dabplus/viterbi47_port.c
)
target_link_libraries(dab_ref_symbol_dump PRIVATE ${DAB_REF_IMPL_AUDIO_LIBS})
target_compile_definitions(dab_ref_symbol_dump PRIVATE ${DAB_REF_IMPL_AUDIO_DEFINES})

target_include_directories(dab_ref_symbol_dump PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ref-impl/dabplus
)

add_executable(dab_msc_ref_compare
    tools/dab_msc_ref_compare.c
    tools/audio_playback.c
    tools/ref_impl_backend_stubs.c
    ref-impl/dabplus/approx.c
    ${DAB_REF_IMPL_AUDIO_SOURCE}
    ref-impl/dabplus/backend_defaults.c
    ref-impl/dabplus/cagc.c
    ref-impl/dabplus/cc.c
    ref-impl/dabplus/ci.c
    ref-impl/dabplus/dab.c
    ref-impl/dabplus/dab_fic.c
    ref-impl/dabplus/fec.c
    ref-impl/dabplus/fi.c
    ref-impl/dabplus/fib.c
    ref-impl/dabplus/fs.c
    ref-impl/dabplus/gf.c
    ref-impl/dabplus/mod.c
    ref-impl/dabplus/ofdm.c
    ref-impl/dabplus/pilot.c
    ref-impl/dabplus/rs_255_245_5.c
    ref-impl/dabplus/sdrplay_api_stub.c
    ref-impl/dabplus/softbit_fft.c
    ref-impl/dabplus/util.c
    ref-impl/dabplus/viterbi47_port.c
)

target_link_libraries(dab_msc_ref_compare PRIVATE dab)
target_link_libraries(dab_msc_ref_compare PRIVATE ${DAB_REF_IMPL_AUDIO_LIBS})
target_compile_definitions(dab_msc_ref_compare PRIVATE ${DAB_REF_IMPL_AUDIO_DEFINES})
if(PkgConfig_FOUND)
    if(PORTAUDIO_FOUND AND (FDK_AAC_FOUND OR FAAD2_FOUND))
        target_compile_definitions(dab_msc_ref_compare PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
        target_link_libraries(dab_msc_ref_compare PRIVATE PkgConfig::PORTAUDIO)
        if(FDK_AAC_FOUND)
            target_compile_definitions(dab_msc_ref_compare PRIVATE DAB_HAVE_FDK_AAC=1)
            target_link_libraries(dab_msc_ref_compare PRIVATE PkgConfig::FDK_AAC)
        endif()
        if(FAAD2_FOUND)
            target_compile_definitions(dab_msc_ref_compare PRIVATE DAB_HAVE_FAAD2=1)
            target_link_libraries(dab_msc_ref_compare PRIVATE PkgConfig::FAAD2)
        endif()
    endif()
endif()
target_include_directories(dab_msc_ref_compare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ref-impl/dabplus
)

add_executable(dab_symbol_dump_compare
    tools/dab_symbol_dump_compare.c
)

add_executable(dab_carrier_dump_compare
    tools/dab_carrier_dump_compare.c
)

add_executable(dab_diff_dump_compare
    tools/dab_diff_dump_compare.c
)

add_executable(dab_active_dump_compare
    tools/dab_active_dump_compare.c
)

add_executable(dab_raw_active_compare
    tools/dab_raw_active_compare.c
)

add_executable(dab_fft_offset_scan_compare
    tools/dab_fft_offset_scan_compare.c
)
target_link_libraries(dab_fft_offset_scan_compare PRIVATE dab m)

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
        tests/test_fic.c
        tests/test_hal.c
        tests/test_msc.c
        tests/test_null_detect.c
        tests/test_ofdm.c
        tests/test_os_shared_lib.c
        tests/test_sdrplay_api.c
        tests/test_sdrplay.c
        tests/test_pc_queue.c
        tests/test_simd.c
        tests/test_prs_acquire.c
        tests/test_prs_fft.c
        tests/test_prs_ref.c
        tests/test_prs_score.c
        tests/test_prs_template.c
        tests/test_sync.c
        tests/test_tframe.c
    )

    target_link_libraries(dab_tests PRIVATE dab)
    target_include_directories(dab_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/sdrplay)

    add_test(NAME dab_tests COMMAND dab_tests)
endif()
