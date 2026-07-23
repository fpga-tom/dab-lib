cmake_minimum_required(VERSION 3.21)

project(dab_lib C CXX)

set(DAB_APP_VERSION "0.1.3")
set(DAB_CAPTURE_BLOCK_COUNT 32 CACHE STRING "Preallocated IQ blocks per open SDR stream")
if(DAB_CAPTURE_BLOCK_COUNT LESS 2)
    message(FATAL_ERROR "DAB_CAPTURE_BLOCK_COUNT must be at least 2")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAB_BUILD_TESTS "Build unit tests" ON)
option(DAB_BUILD_GUI "Build wxWidgets GUI prototype" ON)
option(DAB_BUILD_TOOLS "Build command-line tools" ON)
option(DAB_ENABLE_AVX2 "Enable AVX2 backend when supported" ON)
option(DAB_ENABLE_NEON "Enable NEON backend when supported" ON)
option(DAB_ENABLE_RELEASE_IPO "Enable interprocedural optimization in Release builds" ON)
option(DAB_USE_BUNDLED_WINDOWS_DEPS "Use third-party libraries from libs/ on Windows" ON)

if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

if(DAB_ENABLE_RELEASE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT DAB_RELEASE_IPO_SUPPORTED
                        OUTPUT DAB_RELEASE_IPO_ERROR
                        LANGUAGES C CXX)
    if(DAB_RELEASE_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "Release IPO is unavailable: ${DAB_RELEASE_IPO_ERROR}")
    endif()
endif()

find_package(PkgConfig QUIET)

if(MSVC)
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(DAB_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)
set(DAB_LIBS_INCLUDE_DIR ${DAB_LIBS_DIR}/include)

function(dab_target_link_math target_name)
    if(NOT WIN32)
        target_link_libraries(${target_name} PRIVATE m)
    endif()
endfunction()

function(dab_copy_runtime_dlls target_name)
    if(WIN32 AND ARGN)
        set(runtime_list_file "${CMAKE_CURRENT_BINARY_DIR}/${target_name}-$<CONFIG>-runtime-dlls.cmake")
        dab_generate_runtime_dll_list("${runtime_list_file}" ${ARGN})

        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -Dtarget_dir=$<TARGET_FILE_DIR:${target_name}>
                -Druntime_list_file=${runtime_list_file}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
            VERBATIM
        )
    endif()
endfunction()

function(dab_generate_runtime_dll_list output_file)
    set(runtime_list_content "set(runtime_dlls\n")
    foreach(runtime_dll IN LISTS ARGN)
        string(APPEND runtime_list_content "    \"${runtime_dll}\"\n")
    endforeach()
    string(APPEND runtime_list_content ")\n")
    file(GENERATE OUTPUT "${output_file}" CONTENT "${runtime_list_content}")
endfunction()

function(dab_install_runtime_dlls component_name)
    if(WIN32 AND ARGN)
        set(runtime_list_file "${CMAKE_CURRENT_BINARY_DIR}/install-${component_name}-$<CONFIG>-runtime-dlls.cmake")
        dab_generate_runtime_dll_list("${runtime_list_file}" ${ARGN})
        install(CODE
"set(target_dir \"\${CMAKE_INSTALL_PREFIX}\")
set(runtime_list_file \"${CMAKE_CURRENT_BINARY_DIR}/install-${component_name}-\${CMAKE_INSTALL_CONFIG_NAME}-runtime-dlls.cmake\")
include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake\")"
            COMPONENT ${component_name}
        )
    endif()
endfunction()

function(dab_set_windows_stack_size target_name stack_bytes)
    if(WIN32 AND MSVC)
        target_link_options(${target_name} PRIVATE "/STACK:${stack_bytes}")
    endif()
endfunction()

set(DAB_AUDIO_PLAYBACK_AVAILABLE OFF)
set(DAB_AUDIO_DECODER_AVAILABLE OFF)
set(DAB_AUDIO_OUTPUT_AVAILABLE OFF)
set(DAB_AUDIO_DECODER_LINK_TARGETS "")
set(DAB_AUDIO_RUNTIME_DLLS "")
set(DAB_WXWIDGETS_FOUND OFF)
set(DAB_WXWIDGETS_TARGET "")
set(DAB_WXWIDGETS_RUNTIME_DLLS "")

if(WIN32 AND DAB_USE_BUNDLED_WINDOWS_DEPS)
    if(EXISTS ${DAB_LIBS_DIR}/portaudio_x64.lib AND
       EXISTS ${DAB_LIBS_DIR}/portaudio_x64.dll AND
       EXISTS ${DAB_LIBS_INCLUDE_DIR}/portaudio.h)
        add_library(dab_portaudio SHARED IMPORTED GLOBAL)
        set_target_properties(dab_portaudio PROPERTIES
            IMPORTED_IMPLIB ${DAB_LIBS_DIR}/portaudio_x64.lib
            IMPORTED_LOCATION ${DAB_LIBS_DIR}/portaudio_x64.dll
            INTERFACE_INCLUDE_DIRECTORIES ${DAB_LIBS_INCLUDE_DIR}
        )
        list(APPEND DAB_AUDIO_RUNTIME_DLLS ${DAB_LIBS_DIR}/portaudio_x64.dll)
        set(DAB_PORTAUDIO_FOUND ON)
    endif()

    if(EXISTS ${DAB_LIBS_DIR}/libfaad2_dll.lib AND
       EXISTS ${DAB_LIBS_DIR}/libfaad2_dll.dll AND
       EXISTS ${DAB_LIBS_INCLUDE_DIR}/neaacdec.h)
        add_library(dab_faad2 SHARED IMPORTED GLOBAL)
        set_target_properties(dab_faad2 PROPERTIES
            IMPORTED_IMPLIB ${DAB_LIBS_DIR}/libfaad2_dll.lib
            IMPORTED_LOCATION ${DAB_LIBS_DIR}/libfaad2_dll.dll
            INTERFACE_INCLUDE_DIRECTORIES ${DAB_LIBS_INCLUDE_DIR}
        )
        list(APPEND DAB_AUDIO_DECODER_LINK_TARGETS dab_faad2)
        list(APPEND DAB_AUDIO_RUNTIME_DLLS ${DAB_LIBS_DIR}/libfaad2_dll.dll)
        set(DAB_FAAD2_FOUND ON)
        set(DAB_AUDIO_DECODER_AVAILABLE ON)
    endif()

    if(EXISTS ${DAB_LIBS_INCLUDE_DIR}/wx/wx.h)
        set(DAB_WXWIDGETS_SETUP_SHIM_DIR ${CMAKE_CURRENT_BINARY_DIR}/wxwidgets-setup-shim)
        file(MAKE_DIRECTORY ${DAB_WXWIDGETS_SETUP_SHIM_DIR}/wx)
        file(WRITE ${DAB_WXWIDGETS_SETUP_SHIM_DIR}/wx/setup.h
"#ifndef DAB_WX_SETUP_SHIM_H
#define DAB_WX_SETUP_SHIM_H

#include <wx/msw/setup.h>

#endif
")

        add_library(dab_wxwidgets INTERFACE)
        target_include_directories(dab_wxwidgets INTERFACE
            ${DAB_WXWIDGETS_SETUP_SHIM_DIR}
            ${DAB_LIBS_INCLUDE_DIR}
        )
        target_compile_definitions(dab_wxwidgets INTERFACE __WXMSW__ WXUSINGDLL UNICODE _UNICODE)
        target_link_libraries(dab_wxwidgets INTERFACE
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_adv.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_adv.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_core.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_core.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_gl.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_gl.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxbase32ud.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxbase32u.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxpngd.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxpng.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxjpegd.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxjpeg.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxtiffd.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxtiff.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxzlibd.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxzlib.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxregexud.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxregexu.lib>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxexpatd.lib>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxexpat.lib>
            comctl32
            rpcrt4
            shlwapi
            uxtheme
            version
            winmm
            imm32
            oleacc
            ole32
            uuid
        )
        set(DAB_WXWIDGETS_FOUND ON)
        set(DAB_WXWIDGETS_TARGET dab_wxwidgets)
        set(DAB_WXWIDGETS_RUNTIME_DLLS
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxbase32ud_vc14x_x64.dll>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxbase32u_vc14x_x64.dll>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_adv_vc14x_x64.dll>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_adv_vc14x_x64.dll>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_core_vc14x_x64.dll>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_core_vc14x_x64.dll>
            $<$<CONFIG:Debug>:${DAB_LIBS_DIR}/wxmsw32ud_gl_vc14x_x64.dll>
            $<$<NOT:$<CONFIG:Debug>>:${DAB_LIBS_DIR}/wxmsw32u_gl_vc14x_x64.dll>
        )
    endif()
elseif(PkgConfig_FOUND)
    pkg_check_modules(FDK_AAC IMPORTED_TARGET QUIET fdk-aac)
    pkg_check_modules(FAAD2 IMPORTED_TARGET QUIET faad2)
    pkg_check_modules(PORTAUDIO IMPORTED_TARGET QUIET portaudio-2.0)

    if(PORTAUDIO_FOUND)
        set(DAB_PORTAUDIO_FOUND ON)
    endif()
    if(FDK_AAC_FOUND)
        list(APPEND DAB_AUDIO_DECODER_LINK_TARGETS PkgConfig::FDK_AAC)
        set(DAB_AUDIO_DECODER_AVAILABLE ON)
    endif()
    if(FAAD2_FOUND)
        list(APPEND DAB_AUDIO_DECODER_LINK_TARGETS PkgConfig::FAAD2)
        set(DAB_AUDIO_DECODER_AVAILABLE ON)
    endif()
endif()

set(DAB_AUDIO_OUTPUT_SOURCES
    src/audio/audio_output.c
)
if(DAB_PORTAUDIO_FOUND)
    list(APPEND DAB_AUDIO_OUTPUT_SOURCES src/audio/audio_output_portaudio.c)
    set(DAB_AUDIO_OUTPUT_AVAILABLE ON)
endif()
if(WIN32)
    list(APPEND DAB_AUDIO_OUTPUT_SOURCES src/audio/audio_output_wasapi.c)
    set(DAB_AUDIO_OUTPUT_AVAILABLE ON)
endif()

add_library(dab_audio_output STATIC ${DAB_AUDIO_OUTPUT_SOURCES})
target_include_directories(dab_audio_output
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/audio
)
if(DAB_PORTAUDIO_FOUND)
    target_compile_definitions(dab_audio_output PRIVATE DAB_AUDIO_OUTPUT_HAVE_PORTAUDIO=1)
    if(WIN32 AND DAB_USE_BUNDLED_WINDOWS_DEPS)
        target_link_libraries(dab_audio_output PRIVATE dab_portaudio)
    else()
        target_link_libraries(dab_audio_output PRIVATE PkgConfig::PORTAUDIO)
    endif()
endif()
if(WIN32)
    target_compile_definitions(dab_audio_output PRIVATE DAB_AUDIO_OUTPUT_HAVE_WASAPI=1)
    target_link_libraries(dab_audio_output PRIVATE ole32)
endif()

set(DAB_AUDIO_LINK_TARGETS dab_audio_output ${DAB_AUDIO_DECODER_LINK_TARGETS})
if(DAB_AUDIO_DECODER_AVAILABLE AND DAB_AUDIO_OUTPUT_AVAILABLE)
    set(DAB_AUDIO_PLAYBACK_AVAILABLE ON)
endif()

add_library(dab STATIC
    src/core/audio_eq.c
    src/core/audio_spectrum.c
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
    src/core/sample_history.c
    src/core/sync.c
    src/core/tframe.c
    src/core/viterbi47_port.c
    src/hal/sdr_device.c
    src/hal/sdrplay/sdrplay_api.c
    src/hal/sdrplay/sdrplay_adapter.c
    src/hal/sdrplay/sdrplay_adapter_real.c
    src/hal/sdrplay/sdrplay_adapter_stub.c
    src/hal/sdrplay/sdrplay_device.c
    src/hal/rtlsdr/rtlsdr_api.c
    src/hal/rtlsdr/rtlsdr_adapter.c
    src/hal/rtlsdr/rtlsdr_adapter_real.c
    src/hal/rtlsdr/rtlsdr_adapter_stub.c
    src/hal/rtlsdr/rtlsdr_device.c
    src/os/os_cond.c
    src/os/os_mutex.c
    src/os/os_shared_lib.c
    src/os/os_thread.c
    src/os/os_time.c
    src/receive/receive.c
    src/scan/scan.c
    src/simd/simd.c
    src/simd/simd_scalar.c
)

target_include_directories(dab
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(dab PUBLIC DAB_CAPTURE_BLOCK_COUNT=${DAB_CAPTURE_BLOCK_COUNT})
set_target_properties(dab PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    target_link_libraries(dab PUBLIC m ${CMAKE_DL_LIBS})
endif()

if(DAB_BUILD_GUI)
    find_package(OpenGL QUIET)
    if(NOT WIN32 OR NOT DAB_USE_BUNDLED_WINDOWS_DEPS)
        find_package(wxWidgets QUIET COMPONENTS core base gl)
        if(wxWidgets_FOUND)
            include(${wxWidgets_USE_FILE})
            set(DAB_WXWIDGETS_FOUND ON)
            set(DAB_WXWIDGETS_TARGET ${wxWidgets_LIBRARIES})
        endif()
    endif()

    if(DAB_WXWIDGETS_FOUND)
        add_executable(dab_gui WIN32
            src/gui/main.cpp
            src/gui/main_frame.cpp
            src/gui/dab_gui_settings.c
            src/gui/embedded_audio_playback_backend.c
            src/gui/embedded_audio_scan_backend.c
            tools/audio_playback.c
        )

        target_include_directories(dab_gui PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
            ${CMAKE_CURRENT_SOURCE_DIR}/tools
        )
        target_compile_definitions(dab_gui PRIVATE DAB_GUI_APP_VERSION="${DAB_APP_VERSION}")

        target_link_libraries(dab_gui PRIVATE dab ${DAB_WXWIDGETS_TARGET})
        if(OpenGL_FOUND)
            target_link_libraries(dab_gui PRIVATE OpenGL::GL)
        endif()
        if(DAB_AUDIO_PLAYBACK_AVAILABLE)
            target_compile_definitions(dab_gui PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
            target_link_libraries(dab_gui PRIVATE ${DAB_AUDIO_LINK_TARGETS})
            if(FDK_AAC_FOUND)
                target_compile_definitions(dab_gui PRIVATE DAB_HAVE_FDK_AAC=1)
            endif()
            if(DAB_FAAD2_FOUND OR FAAD2_FOUND)
                target_compile_definitions(dab_gui PRIVATE DAB_HAVE_FAAD2=1)
            endif()
        endif()
        dab_set_windows_stack_size(dab_gui 8388608)
        dab_copy_runtime_dlls(dab_gui ${DAB_AUDIO_RUNTIME_DLLS} ${DAB_WXWIDGETS_RUNTIME_DLLS})
        install(TARGETS dab_gui RUNTIME DESTINATION . COMPONENT gui)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.md DESTINATION . COMPONENT gui)
        dab_install_runtime_dlls(gui ${DAB_AUDIO_RUNTIME_DLLS} ${DAB_WXWIDGETS_RUNTIME_DLLS})
    else()
        message(WARNING "wxWidgets not found; dab_gui target will not be built")
    endif()
endif()

if(DAB_BUILD_TOOLS)
    add_executable(dab_sdrplay_scan
        tools/dab_sdrplay_scan.c
    )

    target_link_libraries(dab_sdrplay_scan PRIVATE dab)
    dab_target_link_math(dab_sdrplay_scan)

    add_executable(dab_sdrplay_probe
        tools/sdrplay_probe.c
    )

    target_link_libraries(dab_sdrplay_probe PRIVATE dab)

    add_executable(dab_simd_bench
        tools/dab_simd_bench.c
    )
    target_link_libraries(dab_simd_bench PRIVATE dab)

    add_executable(dab_sdrplay_null_probe
        tools/sdrplay_null_probe.c
    )

    target_link_libraries(dab_sdrplay_null_probe PRIVATE dab)

    add_executable(dab_sdrplay_sync_probe
        tools/sdrplay_sync_probe.c
    )

    target_link_libraries(dab_sdrplay_sync_probe PRIVATE dab)
    dab_set_windows_stack_size(dab_sdrplay_sync_probe 8388608)

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
    if(DAB_AUDIO_PLAYBACK_AVAILABLE)
        target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
        target_link_libraries(dab_sync_file_replay PRIVATE ${DAB_AUDIO_LINK_TARGETS})
        target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
        target_link_libraries(dab_sdrplay_audio PRIVATE ${DAB_AUDIO_LINK_TARGETS})
        if(FDK_AAC_FOUND)
            target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_FDK_AAC=1)
            target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_FDK_AAC=1)
        endif()
        if(DAB_FAAD2_FOUND OR FAAD2_FOUND)
            target_compile_definitions(dab_sync_file_replay PRIVATE DAB_HAVE_FAAD2=1)
            target_compile_definitions(dab_sdrplay_audio PRIVATE DAB_HAVE_FAAD2=1)
        endif()
    endif()
    dab_set_windows_stack_size(dab_sdrplay_audio 8388608)
    dab_set_windows_stack_size(dab_sync_file_replay 8388608)
    dab_copy_runtime_dlls(dab_sdrplay_audio ${DAB_AUDIO_RUNTIME_DLLS})
    dab_copy_runtime_dlls(dab_sync_file_replay ${DAB_AUDIO_RUNTIME_DLLS})

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
    target_link_libraries(dab_fft_offset_scan_compare PRIVATE dab)
    dab_target_link_math(dab_fft_offset_scan_compare)
endif()

target_compile_definitions(dab
    PRIVATE
        $<$<BOOL:${DAB_ENABLE_AVX2}>:DAB_ENABLE_AVX2=1>
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
        target_compile_definitions(dab PRIVATE DAB_ENABLE_NEON=1)
        if(NOT MSVC AND NOT APPLE AND NOT ANDROID)
            target_compile_options(dab PRIVATE -mfpu=neon)
        endif()
    endif()
endif()

if(DAB_BUILD_TESTS)
    enable_testing()

    add_executable(dab_tests
        tests/test_audio_eq.c
        tests/test_audio_output.c
        tests/test_main.c
        tests/test_audio_spectrum.c
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
        tests/test_rtlsdr_api.c
        tests/test_rtlsdr.c
        tests/test_sample_history.c
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
    target_link_libraries(dab_tests PRIVATE dab_audio_output)
    target_include_directories(dab_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/sdrplay
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/rtlsdr
    )

    add_test(NAME dab_tests COMMAND dab_tests)
endif()
