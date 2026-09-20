set(TEAPOT_AARCH64_SHADOW_STACK_CONFIG
    "${CMAKE_CURRENT_LIST_DIR}/../include/aarch64_shadow_stack.h"
    CACHE FILEPATH "Shared AArch64 shadow-stack configuration header")
get_filename_component(TEAPOT_AARCH64_SHADOW_STACK_CONFIG
    "${TEAPOT_AARCH64_SHADOW_STACK_CONFIG}" ABSOLUTE)
file(READ "${TEAPOT_AARCH64_SHADOW_STACK_CONFIG}" shadow_stack_config)

# Old cache entries may remain in existing build directories. Never let an
# independent override silently disagree with the header used by Python.
foreach(setting IN ITEMS SIZE CONTROL_OFFSET REPORT_OFFSET)
    set(old_option "TEAPOT_AARCH64_SHADOW_STACK_${setting}")
    string(REGEX MATCH "#define AARCH64_SHADOW_STACK_${setting} ([0-9]+)"
        definition "${shadow_stack_config}")
    if(NOT definition)
        message(FATAL_ERROR "Missing literal AARCH64_SHADOW_STACK_${setting} in shared header")
    endif()
    if(DEFINED ${old_option} AND NOT "${${old_option}}" STREQUAL "${CMAKE_MATCH_1}")
        message(FATAL_ERROR
            "${old_option} disagrees with TEAPOT_AARCH64_SHADOW_STACK_CONFIG. "
            "Select the same configuration header for libcheckpoint and Python.")
    endif()
endforeach()

configure_file("${TEAPOT_AARCH64_SHADOW_STACK_CONFIG}"
    "${CMAKE_CURRENT_BINARY_DIR}/include/aarch64_shadow_stack.h" COPYONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/aarch64_shadow_stack.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT checkpoint-config)
