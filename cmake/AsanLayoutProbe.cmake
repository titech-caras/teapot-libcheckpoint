include(CheckCSourceRuns)

function(teapot_check_asan_layout)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(STATUS "ASan/DIFT mapping compatibility is unverified: no target emulator")
        return()
    endif()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(CMAKE_REQUIRED_FLAGS "-fno-pie")
    set(CMAKE_REQUIRED_LINK_OPTIONS -no-pie -fsanitize=address)
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(CMAKE_REQUIRED_DEFINITIONS "-DDIFT_XOR_MASK=${DIFT_XOR_MASK}")
    foreach(def IN LISTS DIFT_RANGE_DEFS)
        list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${def}")
    endforeach()
    # Test the selected runtime mapping against the linked ASan, not a compiler
    # version guess (Clang and vendor/backported runtimes matter too). The code
    # is intentionally uninstrumented, just like libcheckpoint itself.
    unset(TEAPOT_TEST_ASAN_LAYOUT_COMPATIBLE CACHE)
    check_c_source_runs("
        #include <sys/resource.h>
        #include \"${CMAKE_CURRENT_SOURCE_DIR}/src/dift_support.c\"
        int main(void) {
            struct rlimit no_core = {0, 0};
            setrlimit(RLIMIT_CORE, &no_core);
            void *allocation = malloc(32);
            if (!allocation) return 2;
            *(volatile unsigned char *)allocation = 1;
            map_dift_pages();
            dift_set_mem_tags(allocation, TAG_ATTACKER, 32);
            free(allocation);
            /* No LeakSanitizer/ptrace dependency in a mapping-only probe. */
            _exit(0);
        }" TEAPOT_TEST_ASAN_LAYOUT_COMPATIBLE)
endfunction()
