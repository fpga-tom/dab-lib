cmake_minimum_required(VERSION 3.22.1)

project("adab")

get_filename_component(DAB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

set(DAB_BUILD_GUI OFF CACHE BOOL "Build the Windows WinUI 3 GUI" FORCE)
set(DAB_BUILD_TESTS OFF CACHE BOOL "Build unit tests" FORCE)
set(DAB_BUILD_TOOLS OFF CACHE BOOL "Build command-line tools" FORCE)
set(DAB_ENABLE_AVX2 OFF CACHE BOOL "Enable AVX2 backend when supported" FORCE)
set(DAB_ENABLE_NEON ON CACHE BOOL "Enable NEON backend when supported" FORCE)

add_subdirectory("${DAB_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/dab")

add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        dab
        android
        log)
