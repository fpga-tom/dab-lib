cmake_minimum_required(VERSION 3.21)

project(dab_lib C CXX)

set(DAB_APP_VERSION "0.1.5")
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
option(DAB_BUILD_GUI "Build WinUI 3 GUI" ON)
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
set(DAB_AUDIO_LINK_TARGETS "")
set(DAB_AUDIO_RUNTIME_DLLS "")

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
        list(APPEND DAB_AUDIO_LINK_TARGETS dab_portaudio)
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
        list(APPEND DAB_AUDIO_LINK_TARGETS dab_faad2)
        list(APPEND DAB_AUDIO_RUNTIME_DLLS ${DAB_LIBS_DIR}/libfaad2_dll.dll)
        set(DAB_FAAD2_FOUND ON)
    endif()

    if(DAB_PORTAUDIO_FOUND AND DAB_FAAD2_FOUND)
        set(DAB_AUDIO_PLAYBACK_AVAILABLE ON)
    endif()

elseif(PkgConfig_FOUND)
    pkg_check_modules(FDK_AAC IMPORTED_TARGET QUIET fdk-aac)
    pkg_check_modules(FAAD2 IMPORTED_TARGET QUIET faad2)
    pkg_check_modules(PORTAUDIO IMPORTED_TARGET QUIET portaudio-2.0)

    if(PORTAUDIO_FOUND AND (FDK_AAC_FOUND OR FAAD2_FOUND))
        set(DAB_AUDIO_PLAYBACK_AVAILABLE ON)
        list(APPEND DAB_AUDIO_LINK_TARGETS PkgConfig::PORTAUDIO)
        if(FDK_AAC_FOUND)
            list(APPEND DAB_AUDIO_LINK_TARGETS PkgConfig::FDK_AAC)
        endif()
        if(FAAD2_FOUND)
            list(APPEND DAB_AUDIO_LINK_TARGETS PkgConfig::FAAD2)
            if(WIN32)
                find_file(DAB_FAAD2_RUNTIME_DLL
                    NAMES faad-2.dll libfaad-2.dll libfaad2_dll.dll
                    PATHS "${FAAD2_PREFIX}/bin"
                    NO_DEFAULT_PATH
                )
                if(DAB_FAAD2_RUNTIME_DLL)
                    list(APPEND DAB_AUDIO_RUNTIME_DLLS "${DAB_FAAD2_RUNTIME_DLL}")
                endif()
            endif()
        endif()
        if(WIN32)
            find_file(DAB_PORTAUDIO_RUNTIME_DLL
                NAMES portaudio.dll portaudio_x64.dll
                PATHS "${PORTAUDIO_PREFIX}/bin"
                NO_DEFAULT_PATH
            )
            if(DAB_PORTAUDIO_RUNTIME_DLL)
                list(APPEND DAB_AUDIO_RUNTIME_DLLS "${DAB_PORTAUDIO_RUNTIME_DLL}")
            endif()
        endif()
    endif()
endif()

add_library(dab STATIC
    src/core/audio_eq.c
    src/core/audio_spectrum.c
    src/core/block_pool.c
    src/core/charset.c
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
    src/measurement/measurement_alarm.c
    src/measurement/rf_calibration.c
    src/receive/receive.c
    src/receive/receiver_session.c
    src/scan/scan.c
    src/simd/simd.c
    src/simd/simd_scalar.c
)

target_include_directories(dab
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/receive
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

target_compile_definitions(dab PUBLIC DAB_CAPTURE_BLOCK_COUNT=${DAB_CAPTURE_BLOCK_COUNT})
target_compile_definitions(dab PRIVATE DAB_RECEIVE_NO_MAIN=1)
set_target_properties(dab PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    target_link_libraries(dab PUBLIC m ${CMAKE_DL_LIBS})
endif()

if(DAB_BUILD_GUI)
    if(WIN32 AND MSVC)
        add_library(dab_gui_backend STATIC
            src/gui/dab_gui_settings.c
            src/gui/embedded_audio_playback_backend.c
            src/gui/embedded_audio_scan_backend.c
            src/receive/receiver_session.c
            tools/audio_playback.c
        )

        target_include_directories(dab_gui_backend PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/receive
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
            ${CMAKE_CURRENT_SOURCE_DIR}/tools
        )
        target_compile_definitions(dab_gui_backend PRIVATE
            DAB_GUI_PLAYBACK_EMBEDDED=1
            DAB_RECEIVER_SESSION_EMBEDDED=1
        )
        target_link_libraries(dab_gui_backend PUBLIC dab)
        if(DAB_AUDIO_PLAYBACK_AVAILABLE)
            target_compile_definitions(dab_gui_backend PRIVATE DAB_HAVE_AUDIO_PLAYBACK=1)
            target_link_libraries(dab_gui_backend PUBLIC ${DAB_AUDIO_LINK_TARGETS})
            if(FDK_AAC_FOUND)
                target_compile_definitions(dab_gui_backend PRIVATE DAB_HAVE_FDK_AAC=1)
            endif()
            if(DAB_FAAD2_FOUND OR FAAD2_FOUND)
                target_compile_definitions(dab_gui_backend PRIVATE DAB_HAVE_FAAD2=1)
            endif()
        endif()

        if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND
           EXISTS "${CMAKE_MAKE_PROGRAM}")
            set(DAB_MSBUILD_EXECUTABLE "${CMAKE_MAKE_PROGRAM}")
        else()
            set(DAB_MSBUILD_SEARCH_DIR "${CMAKE_CXX_COMPILER}")
            foreach(_msbuild_search_level RANGE 0 10)
                get_filename_component(DAB_MSBUILD_SEARCH_DIR
                    "${DAB_MSBUILD_SEARCH_DIR}" DIRECTORY)
                if(EXISTS "${DAB_MSBUILD_SEARCH_DIR}/MSBuild/Current/Bin/MSBuild.exe")
                    set(DAB_MSBUILD_EXECUTABLE
                        "${DAB_MSBUILD_SEARCH_DIR}/MSBuild/Current/Bin/MSBuild.exe")
                    break()
                endif()
            endforeach()
            if(NOT DAB_MSBUILD_EXECUTABLE)
                find_program(DAB_MSBUILD_EXECUTABLE MSBuild.exe
                    HINTS "$ENV{VSINSTALLDIR}/MSBuild/Current/Bin"
                )
            endif()
        endif()
        if(NOT DAB_MSBUILD_EXECUTABLE)
            message(FATAL_ERROR
                "The WinUI 3 GUI requires MSBuild.exe from the installed Visual Studio instance")
        endif()

        set(DAB_GUI_MSBUILD_TOOLSET_ARGS)
        if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND CMAKE_VS_PLATFORM_TOOLSET)
            list(APPEND DAB_GUI_MSBUILD_TOOLSET_ARGS
                "/p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}")
        endif()

        set(DAB_GUI_AUDIO_INCLUDE_DIRS)
        set(DAB_GUI_AUDIO_LIBRARY_DIRS)
        set(DAB_GUI_AUDIO_LIBRARIES)
        if(DAB_AUDIO_PLAYBACK_AVAILABLE)
            foreach(audio_package PORTAUDIO FDK_AAC FAAD2)
                if(${audio_package}_FOUND)
                    list(APPEND DAB_GUI_AUDIO_INCLUDE_DIRS ${${audio_package}_INCLUDE_DIRS})
                    list(APPEND DAB_GUI_AUDIO_LIBRARY_DIRS ${${audio_package}_LIBRARY_DIRS})
                    foreach(audio_library IN LISTS ${audio_package}_LIBRARIES)
                        if(audio_library MATCHES "\\.lib$")
                            list(APPEND DAB_GUI_AUDIO_LIBRARIES "${audio_library}")
                        else()
                            list(APPEND DAB_GUI_AUDIO_LIBRARIES "${audio_library}.lib")
                        endif()
                    endforeach()
                endif()
            endforeach()
        endif()
        list(REMOVE_DUPLICATES DAB_GUI_AUDIO_INCLUDE_DIRS)
        list(REMOVE_DUPLICATES DAB_GUI_AUDIO_LIBRARY_DIRS)
        list(REMOVE_DUPLICATES DAB_GUI_AUDIO_LIBRARIES)

        set(DAB_GUI_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/winui/$<CONFIG>")
        set(DAB_GUI_OUTPUT_EXE "${DAB_GUI_OUTPUT_DIR}/dab_gui.exe")
        set(DAB_GUI_MSBUILD_PROPS "${CMAKE_CURRENT_BINARY_DIR}/dab_gui-$<CONFIG>.props")
        set(DAB_GUI_RUNTIME_LIST "${CMAKE_CURRENT_BINARY_DIR}/dab_gui-$<CONFIG>-runtime-dlls.cmake")
        dab_generate_runtime_dll_list("${DAB_GUI_RUNTIME_LIST}" ${DAB_AUDIO_RUNTIME_DLLS})
        file(GENERATE OUTPUT "${DAB_GUI_MSBUILD_PROPS}" CONTENT
"<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">
  <PropertyGroup>
    <DabSourceDir>${CMAKE_CURRENT_SOURCE_DIR}</DabSourceDir>
    <DabBackendLibrary>$<TARGET_FILE:dab_gui_backend></DabBackendLibrary>
    <DabCoreLibrary>$<TARGET_FILE:dab></DabCoreLibrary>
    <DabGuiOutputDir>${DAB_GUI_OUTPUT_DIR}</DabGuiOutputDir>
    <DabGuiIncludeDirs>${CMAKE_CURRENT_SOURCE_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR}/src/gui;${DAB_GUI_AUDIO_INCLUDE_DIRS}</DabGuiIncludeDirs>
    <DabGuiLibraryDirs>${DAB_GUI_AUDIO_LIBRARY_DIRS}</DabGuiLibraryDirs>
    <DabGuiAudioLibraries>${DAB_GUI_AUDIO_LIBRARIES}</DabGuiAudioLibraries>
    <DabGuiAudioEnabled>$<IF:$<BOOL:${DAB_AUDIO_PLAYBACK_AVAILABLE}>,1,0></DabGuiAudioEnabled>
    <DabGuiAppVersion>${DAB_APP_VERSION}</DabGuiAppVersion>
  </PropertyGroup>
</Project>
")

        add_custom_command(
            OUTPUT "${DAB_GUI_OUTPUT_EXE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DAB_GUI_OUTPUT_DIR}"
            COMMAND "${DAB_MSBUILD_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/dab_gui.vcxproj"
                /restore
                /nologo
                "/p:Configuration=$<CONFIG>"
                "/p:Platform=x64"
                ${DAB_GUI_MSBUILD_TOOLSET_ARGS}
                "/p:DabCMakeProps=${DAB_GUI_MSBUILD_PROPS}"
                VERBATIM
            COMMAND "${CMAKE_COMMAND}"
                "-Dtarget_dir=${DAB_GUI_OUTPUT_DIR}"
                "-Druntime_list_file=${DAB_GUI_RUNTIME_LIST}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
            DEPENDS
                dab_gui_backend
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/dab_gui.vcxproj"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/App.xaml"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/App.xaml.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/App.xaml.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/RemoveXamlMetadataSelfReference.ps1"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/dab_gui.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/dab_gui.ico"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/dab_gui.svg"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/resource.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/main_window.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/main_window.hpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/winui/pch.h"
            COMMENT "Building WinUI 3 GUI with MSBuild"
            VERBATIM
        )
        add_custom_target(dab_gui ALL DEPENDS "${DAB_GUI_OUTPUT_EXE}")
        install(DIRECTORY "${DAB_GUI_OUTPUT_DIR}/"
            DESTINATION .
            COMPONENT gui
            FILES_MATCHING PATTERN "*"
        )
        install(FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/INSTALL.md
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            ${CMAKE_CURRENT_SOURCE_DIR}/SOURCE_CODE.md
            ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
            DESTINATION .
            COMPONENT gui
        )
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LICENSES
                DESTINATION .
                COMPONENT gui)
        dab_install_runtime_dlls(gui ${DAB_AUDIO_RUNTIME_DLLS})
    else()
        message(WARNING "The WinUI 3 GUI requires Windows and the MSVC toolchain")
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
        src/receive/receiver_session.c
        tools/audio_playback.c
    )

    target_include_directories(dab_sdrplay_audio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/receive
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
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
    dab_target_link_math(dab_gen_mode_i_prs_table)

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

    dab_target_link_math(dab_prs_active_compare)
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
    dab_target_link_math(dab_symbol_dump_compare)

    add_executable(dab_carrier_dump_compare
        tools/dab_carrier_dump_compare.c
    )
    dab_target_link_math(dab_carrier_dump_compare)

    add_executable(dab_diff_dump_compare
        tools/dab_diff_dump_compare.c
    )
    dab_target_link_math(dab_diff_dump_compare)

    add_executable(dab_active_dump_compare
        tools/dab_active_dump_compare.c
    )
    dab_target_link_math(dab_active_dump_compare)

    add_executable(dab_raw_active_compare
        tools/dab_raw_active_compare.c
    )
    dab_target_link_math(dab_raw_active_compare)

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
        tests/test_main.c
        tests/test_charset.c
        tests/test_audio_spectrum.c
        tests/test_block_pool.c
        tests/test_frontend.c
        tests/test_fic.c
        tests/test_hal.c
        tests/test_msc.c
        tests/test_measurement_alarm.c
        tests/test_measurement_session_recorder.cpp
        tests/test_rf_calibration.c
        tests/test_null_detect.c
        tests/test_ofdm.c
        tests/test_os_shared_lib.c
        tests/test_sdrplay_api.c
        tests/test_sdrplay.c
        tests/test_rtlsdr_api.c
        tests/test_rtlsdr.c
        tests/test_receive.c
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
        apps/dab_measurement_winui/MeasurementSessionRecorder.cpp
        apps/dab_measurement_winui/MeasurementSessionReplay.cpp
    )

    target_link_libraries(dab_tests PRIVATE dab)
    dab_set_windows_stack_size(dab_tests 8388608)
    target_include_directories(dab_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/receive
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/sdrplay
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/rtlsdr
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/dab_measurement_winui
    )

    add_test(NAME dab_tests COMMAND dab_tests)
endif()
