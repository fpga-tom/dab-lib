if(NOT DEFINED target_dir OR target_dir STREQUAL "")
    message(FATAL_ERROR "target_dir is required")
endif()

if(DEFINED runtime_list_file AND NOT runtime_list_file STREQUAL "")
    include("${runtime_list_file}")
endif()

if(NOT DEFINED runtime_dlls)
    set(runtime_dlls)
endif()

if(runtime_dlls STREQUAL "")
    return()
endif()

foreach(runtime_dll IN LISTS runtime_dlls)
    if(runtime_dll STREQUAL "")
        continue()
    endif()

    if(EXISTS "${runtime_dll}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_dll}" "${target_dir}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    else()
        message(WARNING "Runtime DLL not found, skipping copy: ${runtime_dll}")
    endif()
endforeach()
