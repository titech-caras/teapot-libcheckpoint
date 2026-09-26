cmake_minimum_required(VERSION 3.16)

project(libcheckpoint C ASM)

include(CTest)

include(GNUInstallDirs)
include(cmake/Options.cmake)
include(cmake/TargetArchitecture.cmake)
include(cmake/DiftLayout.cmake)
include(cmake/AArch64ShadowStack.cmake)
include(cmake/RuntimeTargets.cmake)
include(cmake/DiftWrappers.cmake)

if(BUILD_TESTING)
    include(CheckCSourceCompiles)

    foreach(check IN ITEMS VALID BAD_SIZE BAD_ALIGNMENT REDUCED_ALIGNMENT)
        add_test(NAME checkpoint_protected_object_${check}
            COMMAND "${CMAKE_C_COMPILER}" -fsyntax-only -DTEST_${check}
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/protected_storage.c")
        if(NOT check STREQUAL "VALID")
            set_tests_properties(checkpoint_protected_object_${check} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()

    function(teapot_check_asan_test_link)
        # Some cross toolchains make try_compile build only an archive. These
        # tests need a real executable, including the ASan headers and runtime.
        set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
        set(CMAKE_REQUIRED_FLAGS "-fno-pie")
        set(CMAKE_REQUIRED_LINK_OPTIONS -no-pie -fsanitize=address)
        # Recheck when reconfiguring the same tree with different linker flags.
        unset(TEAPOT_TEST_ASAN_AVAILABLE CACHE)
        check_c_source_compiles("#include <sanitizer/asan_interface.h>
            int main(void) {
                char value = 0;
                __asan_poison_memory_region(&value, sizeof(value));
                return 0;
            }" TEAPOT_TEST_ASAN_AVAILABLE)
    endfunction()

    if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "shadow")
        teapot_check_asan_test_link()
        if(NOT TEAPOT_TEST_ASAN_AVAILABLE)
            message(STATUS "ASan executable linking is unavailable; omitting ASan-only runtime tests")
        endif()
    endif()

    if(CHECKPOINT_ARCH_NAME STREQUAL "aarch64" AND TEAPOT_AARCH64_TAG_STORAGE STREQUAL "mte")
        add_executable(checkpoint_mte_setup_test tests/mte_setup.c asm/storage.S)
        teapot_configure_checkpoint_target(checkpoint_mte_setup_test)
        target_compile_options(checkpoint_mte_setup_test PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(checkpoint_mte_setup_test PRIVATE -Wl,--gc-sections)
        foreach(check IN ITEMS mappings protected)
            add_test(NAME checkpoint_mte_setup_${check}
                COMMAND checkpoint_mte_setup_test ${check})
        endforeach()
    endif()

    add_executable(checkpoint_dift_wrappers_test
        tests/dift_wrappers.c src/dift_wrappers/dift_wrappers.c)
    target_include_directories(checkpoint_dift_wrappers_test PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/include" include)
    target_compile_definitions(checkpoint_dift_wrappers_test PRIVATE DIFT_XOR_MASK=0x70000000ULL)
    target_link_options(checkpoint_dift_wrappers_test PRIVATE -Wl,--wrap=strdup)
    foreach(check IN ITEMS allocations memory strings addresses block-input line-input fortified-failure)
        add_test(NAME checkpoint_dift_wrappers_${check}
            COMMAND checkpoint_dift_wrappers_test ${check})
    endforeach()

    add_executable(checkpoint_report_pages_test
        tests/report_pages.c src/report_gadget.c)
    teapot_configure_checkpoint_target(checkpoint_report_pages_test)
    target_link_options(checkpoint_report_pages_test PRIVATE
        -Wl,--wrap=mprotect -Wl,--wrap=sysconf
        -Wl,--wrap=fopen -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
        -Wl,--wrap=getline -Wl,--wrap=free -Wl,--wrap=fclose
        -Wl,--wrap=fprintf -Wl,--wrap=snprintf -Wl,--wrap=fputs -Wl,--wrap=fputc
        -Wl,--wrap=syscall)
    foreach(check IN ITEMS isolated permissions crossing failure restore-failure missing-tail large-page
                           no-heap no-maps read-error split-maps format long-format disabled)
        add_test(NAME checkpoint_report_pages_${check}
            COMMAND checkpoint_report_pages_test ${check})
    endforeach()

    if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "shadow" AND TEAPOT_TEST_ASAN_AVAILABLE)
        add_executable(checkpoint_report_pages_asan_test tests/report_pages.c src/report_gadget.c)
        teapot_configure_checkpoint_target(checkpoint_report_pages_asan_test)
        target_compile_definitions(checkpoint_report_pages_asan_test PRIVATE REPORT_TEST_ASAN ENABLE_NESTED_SPECULATION)
        target_compile_options(checkpoint_report_pages_asan_test PRIVATE -fno-pie)
        get_target_property(report_test_link_options checkpoint_report_pages_test LINK_OPTIONS)
        target_link_options(checkpoint_report_pages_asan_test PRIVATE ${report_test_link_options} -no-pie -fsanitize=address)
        foreach(check IN ITEMS no-heap split-maps no-maps read-error long-format)
            add_test(NAME checkpoint_report_poisoned_${check}
                COMMAND checkpoint_report_pages_asan_test ${check})
            set_tests_properties(checkpoint_report_poisoned_${check} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
        endforeach()
    endif()

    add_executable(checkpoint_signal_handler_test
        tests/signal_handler.c src/signal_handler.c)
    teapot_configure_checkpoint_target(checkpoint_signal_handler_test)
    add_test(NAME checkpoint_signal_context
        COMMAND checkpoint_signal_handler_test context)
    add_test(NAME checkpoint_signal_forwarding
        COMMAND checkpoint_signal_handler_test forward)
    add_test(NAME checkpoint_signal_return
        COMMAND checkpoint_signal_handler_test redirect)
    add_test(NAME checkpoint_signal_registration
        COMMAND checkpoint_signal_handler_test registration)

    foreach(mode IN ITEMS default nested)
        set(entry_test checkpoint_entry_${mode}_test)
        set(entry_runtime_sources ${CHECKPOINT_RUNTIME_SOURCES})
        if(mode STREQUAL "nested")
            # Exercise protected-section boundaries in both object link orders.
            list(REVERSE entry_runtime_sources)
        endif()
        add_executable(${entry_test}
            tests/checkpoint_entry.c tests/checkpoint_entry.S
            ${entry_runtime_sources})
        teapot_configure_checkpoint_target(${entry_test})
        target_compile_definitions(${entry_test} PRIVATE TIME)
        if(mode STREQUAL "nested")
            target_compile_definitions(${entry_test} PRIVATE ENABLE_NESTED_SPECULATION)
        endif()
        target_compile_options(${entry_test} PRIVATE -fno-pie)
        target_link_options(${entry_test} PRIVATE -no-pie)
        if(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
            target_link_options(${entry_test} PRIVATE -Wl,--no-relax)
        endif()
        foreach(check IN ITEMS capacity timing storage memlog)
            add_test(NAME checkpoint_entry_${mode}_${check}
                COMMAND ${entry_test} ${check})
            set_tests_properties(checkpoint_entry_${mode}_${check}
                PROPERTIES TIMEOUT 10)
        endforeach()
        if(CMAKE_READELF)
            add_test(NAME checkpoint_storage_${mode}_elf
                COMMAND "${CMAKE_READELF}" -SW "$<TARGET_FILE:${entry_test}>")
            set_tests_properties(checkpoint_storage_${mode}_elf PROPERTIES
                PASS_REGULAR_EXPRESSION "teapot_protected_bss[ \t]+NOBITS")
        endif()
        if(CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
            target_sources(${entry_test} PRIVATE
                tests/checkpoint_aarch64_bti.c tests/checkpoint_aarch64_bti.S)
            if(TEAPOT_EXPERIMENTAL_AARCH64_BTI)
                target_sources(${entry_test} PRIVATE tests/checkpoint_aarch64_bti_layout.S)
                target_link_options(${entry_test} PRIVATE
                    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/cmake/AArch64Bti.ld")
                add_test(NAME checkpoint_entry_${mode}_bti_backend
                    COMMAND ${entry_test} bti-backend)
                set_tests_properties(checkpoint_entry_${mode}_bti_backend PROPERTIES
                    TIMEOUT 10 SKIP_RETURN_CODE 77)
                add_test(NAME checkpoint_entry_${mode}_bti_traps
                    COMMAND ${entry_test} bti-traps)
                set_tests_properties(checkpoint_entry_${mode}_bti_traps PROPERTIES
                    TIMEOUT 30 SKIP_RETURN_CODE 77)
                if(mode STREQUAL "nested")
                    add_test(NAME checkpoint_entry_nested_bti_backend_live_chain
                        COMMAND ${entry_test} bti-backend-live-chain)
                    set_tests_properties(checkpoint_entry_nested_bti_backend_live_chain PROPERTIES
                        TIMEOUT 10 SKIP_RETURN_CODE 77)
                endif()
            endif()
            add_test(NAME checkpoint_entry_${mode}_report
                COMMAND ${entry_test} report)
            set_tests_properties(checkpoint_entry_${mode}_report PROPERTIES TIMEOUT 10)
            add_test(NAME checkpoint_entry_${mode}_bti_fault
                COMMAND ${entry_test} bti-fault)
            set_tests_properties(checkpoint_entry_${mode}_bti_fault PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77)
            if(mode STREQUAL "nested")
                add_test(NAME checkpoint_entry_nested_bti_live_chain
                    COMMAND ${entry_test} bti-live-chain)
                set_tests_properties(checkpoint_entry_nested_bti_live_chain PROPERTIES
                    TIMEOUT 10 SKIP_RETURN_CODE 77)
            endif()
        endif()
    endforeach()
endif()

if(BUILD_TESTING AND CHECKPOINT_ARCH_NAME STREQUAL "x64")
    add_executable(checkpoint_x64_simd_normal_path_test
        tests/checkpoint_x64_simd_normal_path.c
        tests/checkpoint_x64_simd_normal_path.S)
    target_link_libraries(checkpoint_x64_simd_normal_path_test PRIVATE checkpoint)
    target_compile_options(checkpoint_x64_simd_normal_path_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_simd_normal_path_test PRIVATE -no-pie)
    add_test(
        NAME checkpoint_x64_simd_normal_path
        COMMAND checkpoint_x64_simd_normal_path_test)
endif()

if(BUILD_TESTING AND CHECKPOINT_ARCH_NAME STREQUAL "x64" AND TEAPOT_TEST_ASAN_AVAILABLE)
    add_executable(checkpoint_x64_xsave_report_test
        tests/checkpoint_x64_xsave_report.c
        tests/checkpoint_x64_xsave_report.S)
    teapot_configure_checkpoint_target(checkpoint_x64_xsave_report_test)
    target_link_libraries(checkpoint_x64_xsave_report_test PRIVATE checkpoint)
    target_compile_options(checkpoint_x64_xsave_report_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_xsave_report_test PRIVATE -no-pie -fsanitize=address)
    add_test(
        NAME checkpoint_x64_xsave_report
        COMMAND checkpoint_x64_xsave_report_test)
    set_tests_properties(
        checkpoint_x64_xsave_report PROPERTIES SKIP_RETURN_CODE 77)
    # An incompatible ASan mapping fails in preinit, before main can return 77.
    if(TEAPOT_DIFT_LAYOUT STREQUAL "x64-la48" AND
       CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
        message(WARNING "Disabling the XSAVE report test: GCC 13+ ASan requires -DTEAPOT_DIFT_LAYOUT=x64-la48-asan-new")
        set_tests_properties(checkpoint_x64_xsave_report PROPERTIES DISABLED TRUE)
    endif()
endif()

if(BUILD_TESTING AND CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
    add_executable(checkpoint_aarch64_report_call_nop_test
        tests/checkpoint_aarch64_report_call_nop.c src/report_gadget.c)
    teapot_configure_checkpoint_target(checkpoint_aarch64_report_call_nop_test)
    # Test the patcher without linking the full runtime's startup mappings.
    target_compile_options(checkpoint_aarch64_report_call_nop_test PRIVATE
        -ffunction-sections -fdata-sections)
    target_link_options(checkpoint_aarch64_report_call_nop_test PRIVATE
        -Wl,--gc-sections)
    add_test(NAME checkpoint_aarch64_report_call_nop
        COMMAND checkpoint_aarch64_report_call_nop_test)
endif()

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN "aarch64_shadow_stack.h" EXCLUDE)
