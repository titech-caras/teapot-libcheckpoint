set(DIFT_RANGE_DEFS)
set(TEAPOT_DIFT_LAYOUT_ARCH)

function(teapot_dift_layout name)
    set(one_value_args ARCH XOR_MASK ASAN_SHADOW_OFFSET DEFAULT_FOR)
    set(multi_value_args APP_RANGES)
    cmake_parse_arguments(LAYOUT "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT LAYOUT_ARCH)
        message(FATAL_ERROR "DIFT layout ${name} does not define ARCH")
    endif()
    if(NOT LAYOUT_XOR_MASK)
        message(FATAL_ERROR "DIFT layout ${name} does not define XOR_MASK")
    endif()
    if(NOT LAYOUT_ASAN_SHADOW_OFFSET)
        message(FATAL_ERROR "DIFT layout ${name} does not define ASAN_SHADOW_OFFSET")
    endif()
    if(NOT LAYOUT_APP_RANGES)
        message(FATAL_ERROR "DIFT layout ${name} does not define APP_RANGES")
    endif()

    set(selected FALSE)
    if("${TEAPOT_DIFT_LAYOUT}" STREQUAL "${name}")
        set(selected TRUE)
    elseif(NOT TEAPOT_DIFT_LAYOUT AND "${LAYOUT_DEFAULT_FOR}" STREQUAL "${CHECKPOINT_ARCH_NAME}")
        set(TEAPOT_DIFT_LAYOUT "${name}" PARENT_SCOPE)
        set(selected TRUE)
    endif()

    if(NOT selected)
        return()
    endif()

    set(range_defs)
    set(range_idx 0)
    foreach(range IN LISTS LAYOUT_APP_RANGES)
        string(REPLACE ":" ";" range_pair "${range}")
        list(LENGTH range_pair range_pair_len)
        if(NOT range_pair_len EQUAL 2)
            message(FATAL_ERROR "Invalid app range ${range} in DIFT layout ${name}")
        endif()
        list(GET range_pair 0 range_start)
        list(GET range_pair 1 range_end)
        list(APPEND range_defs
            DIFT_APP_RANGE${range_idx}_START=${range_start}ULL
            DIFT_APP_RANGE${range_idx}_END=${range_end}ULL)
        math(EXPR range_idx "${range_idx} + 1")
    endforeach()

    set(TEAPOT_DIFT_LAYOUT_ARCH "${LAYOUT_ARCH}" PARENT_SCOPE)
    set(DIFT_XOR_MASK "${LAYOUT_XOR_MASK}ULL" PARENT_SCOPE)
    set(DIFT_ASAN_SHADOW_OFFSET "${LAYOUT_ASAN_SHADOW_OFFSET}ULL" PARENT_SCOPE)
    set(DIFT_RANGE_DEFS "${range_defs}" PARENT_SCOPE)
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/DiftLayoutData.cmake")

if(NOT TEAPOT_DIFT_LAYOUT_ARCH)
    message(FATAL_ERROR "Unsupported Teapot DIFT layout: ${TEAPOT_DIFT_LAYOUT}")
endif()

if(NOT "${TEAPOT_DIFT_LAYOUT_ARCH}" STREQUAL "${CHECKPOINT_ARCH_NAME}")
    message(FATAL_ERROR
        "Teapot DIFT layout ${TEAPOT_DIFT_LAYOUT} is for ${TEAPOT_DIFT_LAYOUT_ARCH}, "
        "but libcheckpoint target is ${CHECKPOINT_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Teapot DIFT layout: ${TEAPOT_DIFT_LAYOUT}")
if(TEAPOT_ENABLE_DIFT_RUNTIME)
    message(STATUS "Teapot DIFT runtime initialization: enabled")
else()
    message(STATUS "Teapot DIFT runtime initialization: disabled")
endif()
