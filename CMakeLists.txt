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
    # Archive visibility does not determine ownership: hidden and weak global
    # definitions are runtime-owned as well. Never omit this coverage gate just
    # because the capture tool or manifest is unavailable.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT CMAKE_NM)
        message(FATAL_ERROR "CMAKE_NM is required for runtime export coverage")
    endif()
    foreach(archive IN ITEMS checkpoint checkpoint_nested
                             checkpoint_dift_math_wrappers checkpoint_dift_zlib_wrappers)
        if(TARGET ${archive})
            add_test(NAME ${archive}_reserved_exports
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_exports.py"
                    --manifest "${CMAKE_CURRENT_SOURCE_DIR}/tools/runtime_exports.json"
                    --nm "${CMAKE_NM}" --archive "$<TARGET_FILE:${archive}>")
        endif()
    endforeach()
    include(CheckCSourceCompiles)
    add_executable(checkpoint_fault_sites_test tests/fault_sites.c tests/fault_sites.S src/fault_sites.c src/fault_x64.c)
    teapot_configure_checkpoint_target(checkpoint_fault_sites_test)
    target_compile_definitions(checkpoint_fault_sites_test PRIVATE ENABLE_FAULT_TRAINING)
    target_compile_options(checkpoint_fault_sites_test PRIVATE -fno-pie)
    target_link_options(checkpoint_fault_sites_test PRIVATE -no-pie -Wl,-z,separate-code)
    foreach(check IN ITEMS helpers registry off malformed nested)
        add_test(NAME checkpoint_fault_sites_${check} COMMAND checkpoint_fault_sites_test ${check})
        set_tests_properties(checkpoint_fault_sites_${check} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(checkpoint_fault_startup_test tests/fault_sites.c tests/fault_sites.S src/fault_sites.c src/fault_x64.c)
    teapot_configure_checkpoint_target(checkpoint_fault_startup_test)
    target_compile_definitions(checkpoint_fault_startup_test PRIVATE ENABLE_FAULT_TRAINING TEST_FAULT_STARTUP)
    target_compile_options(checkpoint_fault_startup_test PRIVATE -fno-pie)
    target_link_options(checkpoint_fault_startup_test PRIVATE -no-pie -Wl,-z,separate-code)
    foreach(symbol IN ITEMS malloc calloc realloc free fopen fdopen getline qsort mmap)
        target_link_options(checkpoint_fault_startup_test PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
    foreach(check IN ITEMS startup-on startup-off)
        add_test(NAME checkpoint_fault_${check} COMMAND checkpoint_fault_startup_test ${check})
        set_tests_properties(checkpoint_fault_${check} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(checkpoint_fault_startup_parser_test tests/fault_startup_parser.c tests/fault_sites.S src/fault_x64.c)
    teapot_configure_checkpoint_target(checkpoint_fault_startup_parser_test)
    target_compile_definitions(checkpoint_fault_startup_parser_test PRIVATE ENABLE_FAULT_TRAINING)
    target_compile_options(checkpoint_fault_startup_parser_test PRIVATE -fno-pie)
    target_link_options(checkpoint_fault_startup_parser_test PRIVATE -no-pie -Wl,-z,separate-code)
    add_test(NAME checkpoint_fault_startup-parser COMMAND checkpoint_fault_startup_parser_test)
    set_tests_properties(checkpoint_fault_startup-parser PROPERTIES TIMEOUT 30)
    add_test(NAME checkpoint_fault_startup-storage COMMAND checkpoint_fault_startup_parser_test storage)
    set_tests_properties(checkpoint_fault_startup-storage PROPERTIES TIMEOUT 30)
    if(NOT CMAKE_READELF)
        message(FATAL_ERROR "CMAKE_READELF is required for the startup NOBITS regression")
    endif()
    add_test(NAME checkpoint_fault_startup-storage-elf
        COMMAND "${CMAKE_COMMAND}" "-DREADELF=${CMAKE_READELF}"
            "-DBINARY=$<TARGET_FILE:checkpoint_fault_startup_parser_test>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fault_startup_storage.cmake")

    add_test(NAME checkpoint_dift_layout_range_limit
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dift_layout_range_limit.cmake")
    add_test(NAME checkpoint_optional_zlib_required
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/optional_zlib_required.cmake")
    if(CMAKE_NM)
        foreach(archive IN ITEMS checkpoint checkpoint_nested
                                 checkpoint_dift_math_wrappers checkpoint_dift_zlib_wrappers)
            if(NOT TARGET ${archive})
                continue()
            endif()
            set(runtime OFF)
            if(archive MATCHES "^checkpoint(_nested)?$")
                set(runtime ON)
            endif()
            add_test(NAME ${archive}_protected_globals
                COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:${archive}>"
                    "-DRUNTIME=${runtime}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_protected_globals.cmake")
        endforeach()
    else()
        message(WARNING "CMAKE_NM is not set; the protected-globals tests are not registered")
    endif()

    # The start-up contract check (runtime_contract.h). Each program links the
    # runtime with one module record built with DEFINITIONS, or with none, and
    # a constructor that a refused program must never reach.
    function(teapot_add_contract_test name expected)
        cmake_parse_arguments(CONTRACT "NO_RECORD;ACCEPTED" "" "DEFINITIONS;SOURCES" ${ARGN})
        set(target checkpoint_contract_${name}_test)
        set(sources tests/contract_check.c ${CHECKPOINT_RUNTIME_SOURCES})
        if(TEAPOT_ENABLE_COVERAGE)
            list(APPEND sources tests/coverage_provider.c)
        endif()
        if(NOT CONTRACT_NO_RECORD)
            list(APPEND sources tests/contract_module_record.c)
        endif()
        list(APPEND sources ${CONTRACT_SOURCES})
        add_executable(${target} ${sources})
        teapot_configure_checkpoint_target(${target})
        target_compile_definitions(${target} PRIVATE ${CONTRACT_DEFINITIONS})
        target_compile_options(${target} PRIVATE -fno-pie)
        target_link_options(${target} PRIVATE -no-pie)
        if(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
            target_link_options(${target} PRIVATE -Wl,--no-relax)
        endif()
        if(CONTRACT_ACCEPTED)
            add_test(NAME checkpoint_contract_${name} COMMAND ${target})
            set_tests_properties(checkpoint_contract_${name} PROPERTIES
                PASS_REGULAR_EXPRESSION "${expected}")
        else()
            add_test(NAME checkpoint_contract_${name}
                COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:${target}>"
                    "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}" "-DEXPECTED=${expected}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_refusal.cmake")
        endif()
        set_tests_properties(checkpoint_contract_${name} PROPERTIES TIMEOUT 60)
    endfunction()
    teapot_add_contract_test(accepted "contract accepted" ACCEPTED)
    if(TEAPOT_ENABLE_FAULT_TRAINING)
        teapot_add_contract_test(fault_training "contract accepted" ACCEPTED
            DEFINITIONS TEST_CONTRACT_FAULT_TABLE TEST_CONTRACT_REQUIRED=LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING
            SOURCES tests/fault_sites.S)
        teapot_add_contract_test(fault_missing_table "table/capability disagreement"
            DEFINITIONS TEST_CONTRACT_REQUIRED=LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING)
    else()
        teapot_add_contract_test(fault_training "capabilities this archive lacks: fault_training"
            DEFINITIONS TEST_CONTRACT_FAULT_TABLE TEST_CONTRACT_REQUIRED=LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING
            SOURCES tests/fault_sites.S)
    endif()
    teapot_add_contract_test(fault_unrequired_table "table/capability disagreement"
        DEFINITIONS TEST_CONTRACT_FAULT_TABLE SOURCES tests/fault_sites.S)
    teapot_add_contract_test(nested_accepted "contract accepted" ACCEPTED
        DEFINITIONS ENABLE_NESTED_SPECULATION
            TEST_CONTRACT_REQUIRED=LIBCHECKPOINT_CAPABILITY_NESTED)
    teapot_add_contract_test(missing "carries no Teapot contract record" NO_RECORD)
    teapot_add_contract_test(fingerprint "a runtime with another ABI"
        DEFINITIONS TEST_CONTRACT_FINGERPRINT=1)
    teapot_add_contract_test(version "another contract version"
        DEFINITIONS TEST_CONTRACT_VERSION=999)
    teapot_add_contract_test(capability "capabilities this archive lacks: nested"
        DEFINITIONS TEST_CONTRACT_REQUIRED=LIBCHECKPOINT_CAPABILITY_NESTED)
    teapot_add_contract_test(anchor "does not refer to this runtime"
        DEFINITIONS TEST_CONTRACT_WRONG_ANCHOR)
    teapot_add_contract_test(magic "malformed module contract record"
        DEFINITIONS TEST_CONTRACT_MAGIC=0x12345678)
    teapot_add_contract_test(second_record "a runtime with another ABI"
        SOURCES tests/contract_second_record.c)
    # Malformed record layouts: the JSON runs past the section, the section ends
    # inside a record's padding, and a record off the 8-byte grid.
    teapot_add_contract_test(truncated "truncated module contract record"
        DEFINITIONS TEST_CONTRACT_JSON_SIZE=4096)
    teapot_add_contract_test(truncated_padding "truncated module contract record"
        DEFINITIONS TEST_CONTRACT_JSON_SIZE=1 TEST_CONTRACT_JSON_BYTES=1)
    teapot_add_contract_test(mixed_alignment "(truncated|malformed) module contract record" NO_RECORD
        SOURCES tests/contract_mixed_alignment.c)
    # Each manifest states the coverage mode the build was configured with, in
    # the fingerprinted ABI and as the capability.
    foreach(archive IN ITEMS checkpoint checkpoint_nested)
        if(TARGET ${archive})
            add_test(NAME ${archive}_contract_coverage_mode
                COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/lib${archive}.contract.json"
                    "-DENABLED=${TEAPOT_ENABLE_COVERAGE}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_coverage_mode.cmake")
            # A coverage archive leaves its callbacks undefined, with no weak
            # fallback, so that a link without a provider fails; an ordinary
            # archive does not refer to them.
            if(CMAKE_NM)
                add_test(NAME ${archive}_coverage_references
                    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:${archive}>"
                        "-DENABLED=${TEAPOT_ENABLE_COVERAGE}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_coverage_references.cmake")
            endif()
        endif()
    endforeach()

    add_executable(checkpoint_dift_mapping_test tests/dift_mapping.c)
    teapot_configure_checkpoint_target(checkpoint_dift_mapping_test)
    add_test(NAME checkpoint_dift_mapping COMMAND checkpoint_dift_mapping_test)

    foreach(check IN ITEMS VALID BAD_SIZE BAD_ALIGNMENT REDUCED_ALIGNMENT)
        add_test(NAME checkpoint_protected_object_${check}
            COMMAND "${CMAKE_C_COMPILER}" -fsyntax-only -DTEST_${check}
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/protected_storage.c")
        if(NOT check STREQUAL "VALID")
            set_tests_properties(checkpoint_protected_object_${check} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()

    if(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
        # This supported configuration has prioritized depth but no initial
        # full-depth burst. In particular it must handle counter wrap to zero.
        add_executable(checkpoint_entry_priority_test
            tests/checkpoint_entry.c tests/checkpoint_entry.S tests/contract_module_record.c
            tests/coverage_provider.c ${CHECKPOINT_RUNTIME_SOURCES})
        teapot_configure_checkpoint_target(checkpoint_entry_priority_test)
        target_compile_definitions(checkpoint_entry_priority_test PRIVATE
            SPECFUZZ_PRIORITIZED_SIMULATION TIME COVERAGE_PROVIDER_WITHOUT_GUARD)
        target_compile_options(checkpoint_entry_priority_test PRIVATE -fno-pie)
        target_link_options(checkpoint_entry_priority_test PRIVATE -no-pie -Wl,--no-relax)
        add_test(NAME checkpoint_entry_priority_capacity
            COMMAND checkpoint_entry_priority_test capacity)
        set_tests_properties(checkpoint_entry_priority_capacity PROPERTIES TIMEOUT 10)
    endif()

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
        # mte_setup.c includes checkpoint.c, whose start-up check needs this
        # archive's runtime record and a module record.
        add_executable(checkpoint_mte_setup_test tests/mte_setup.c asm/storage.S src/fault_sites.c src/fault_x64.c
            "${LIBCHECKPOINT_CONTRACT_RECORD_SOURCE}" tests/contract_module_record.c)
        teapot_configure_checkpoint_target(checkpoint_mte_setup_test)
        if(TEAPOT_ENABLE_COVERAGE)
            target_sources(checkpoint_mte_setup_test PRIVATE tests/coverage_provider.c)
        endif()
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
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0" TIMEOUT 30)
        endforeach()
    endif()

    add_executable(checkpoint_signal_handler_test
        tests/signal_handler.c src/signal_handler.c src/fault_sites.c src/fault_x64.c)
    teapot_configure_checkpoint_target(checkpoint_signal_handler_test)
    target_link_options(checkpoint_signal_handler_test PRIVATE
        -Wl,--wrap=fprintf -Wl,--wrap=strsignal)
    add_test(NAME checkpoint_signal_context
        COMMAND checkpoint_signal_handler_test context)
    add_test(NAME checkpoint_signal_forwarding
        COMMAND checkpoint_signal_handler_test forward)
    add_test(NAME checkpoint_signal_return
        COMMAND checkpoint_signal_handler_test redirect)
    add_test(NAME checkpoint_signal_registration
        COMMAND checkpoint_signal_handler_test registration)
    add_test(NAME checkpoint_signal_unmanaged
        COMMAND checkpoint_signal_handler_test unmanaged)
    add_test(NAME checkpoint_signal_user_faults
        COMMAND checkpoint_signal_handler_test user-faults)
    add_executable(checkpoint_fault_signal_test
        tests/signal_handler.c tests/fault_sites.S src/signal_handler.c src/fault_sites.c src/fault_x64.c)
    teapot_configure_checkpoint_target(checkpoint_fault_signal_test)
    target_compile_definitions(checkpoint_fault_signal_test PRIVATE ENABLE_FAULT_TRAINING FAULT_SIGNAL_TEST)
    target_compile_options(checkpoint_fault_signal_test PRIVATE -fno-pie)
    target_link_options(checkpoint_fault_signal_test PRIVATE -no-pie -Wl,-z,separate-code
        -Wl,--wrap=fprintf -Wl,--wrap=strsignal)
    add_test(NAME checkpoint_fault_signal COMMAND checkpoint_fault_signal_test training)
    set_tests_properties(checkpoint_fault_signal PROPERTIES TIMEOUT 30)
    if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "shadow" AND TEAPOT_TEST_ASAN_AVAILABLE)
        # The runtime is not ASan-instrumented, but ASan's interceptors check the
        # poisoned protected state passed to them. Build the handler the way that
        # turns a struct sigaction copy into a memcpy call: GCC does so at -Os on
        # AArch64 and RISC-V, and on x64 with the libcall string strategy; clang
        # does so at -O0 (and at -O2 on x64 and RISC-V).
        add_executable(checkpoint_signal_handler_asan_test
            tests/signal_handler.c src/signal_handler.c src/fault_sites.c src/fault_x64.c)
        teapot_configure_checkpoint_target(checkpoint_signal_handler_asan_test)
        target_compile_definitions(checkpoint_signal_handler_asan_test PRIVATE SIGNAL_TEST_ASAN)
        target_compile_options(checkpoint_signal_handler_asan_test PRIVATE -fno-pie)
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(checkpoint_signal_handler_asan_test PRIVATE -Os)
            if(CHECKPOINT_ARCH_NAME STREQUAL "x64")
                target_compile_options(checkpoint_signal_handler_asan_test PRIVATE
                    -mstringop-strategy=libcall)
            endif()
        else()
            target_compile_options(checkpoint_signal_handler_asan_test PRIVATE -O0)
        endif()
        target_link_options(checkpoint_signal_handler_asan_test PRIVATE
            -Wl,--wrap=fprintf -Wl,--wrap=strsignal -no-pie -fsanitize=address)
        # A timeout, because an ASan report can stall in clang's external
        # symbolizer instead of failing.
        foreach(check IN ITEMS forward registration user-faults)
            add_test(NAME checkpoint_signal_poisoned_${check}
                COMMAND checkpoint_signal_handler_asan_test ${check})
            set_tests_properties(checkpoint_signal_poisoned_${check} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0" TIMEOUT 30)
        endforeach()
    endif()

    foreach(mode IN ITEMS default nested)
        set(entry_test checkpoint_entry_${mode}_test)
        set(entry_runtime_sources ${CHECKPOINT_RUNTIME_SOURCES})
        if(mode STREQUAL "nested")
            # Exercise protected-section boundaries in both object link orders.
            list(REVERSE entry_runtime_sources)
        endif()
        add_executable(${entry_test}
            tests/checkpoint_entry.c tests/checkpoint_entry.S tests/contract_module_record.c
            ${entry_runtime_sources})
        teapot_configure_checkpoint_target(${entry_test})
        target_compile_definitions(${entry_test} PRIVATE TIME COVERAGE)
        # COVERAGE runtimes require their coverage callbacks.
        target_sources(${entry_test} PRIVATE tests/coverage_provider.c)
        target_compile_definitions(${entry_test} PRIVATE COVERAGE_PROVIDER_WITHOUT_GUARD)
        if(CHECKPOINT_ARCH_NAME STREQUAL "x64")
            target_compile_definitions(${entry_test} PRIVATE COVERAGE_PROVIDER_WITHOUT_TRACE_PC)
            target_sources(${entry_test} PRIVATE tests/checkpoint_x64_df.c tests/checkpoint_x64_df.S)
            target_link_options(${entry_test} PRIVATE -Wl,--wrap=restore_checkpoint)
            add_test(NAME checkpoint_entry_${mode}_df COMMAND ${entry_test} df)
            set_tests_properties(checkpoint_entry_${mode}_df PROPERTIES TIMEOUT 10)
        endif()
        if(mode STREQUAL "nested")
            target_compile_definitions(${entry_test} PRIVATE ENABLE_NESTED_SPECULATION)
        endif()
        target_compile_options(${entry_test} PRIVATE -fno-pie)
        target_link_options(${entry_test} PRIVATE -no-pie)
        if(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
            target_link_options(${entry_test} PRIVATE -Wl,--no-relax)
            target_sources(${entry_test} PRIVATE
                tests/checkpoint_riscv64_float.c tests/checkpoint_riscv64_float.S)
            # Registered either way: without the FP state the rollback leaves
            # the speculative values, and the test must notice.
            add_test(NAME checkpoint_entry_${mode}_riscv64_float COMMAND ${entry_test} riscv64-float)
            set_tests_properties(checkpoint_entry_${mode}_riscv64_float PROPERTIES TIMEOUT 10)
            if(NOT TEAPOT_ENABLE_RISCV_FLOAT_STATE)
                set_tests_properties(checkpoint_entry_${mode}_riscv64_float PROPERTIES WILL_FAIL TRUE)
            endif()
        endif()
        foreach(check IN ITEMS capacity timing storage memlog memlog-validation memlog-order queued-tags coverage
                               assertions)
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
                target_sources(${entry_test} PRIVATE
                    tests/checkpoint_aarch64_pac.c tests/checkpoint_aarch64_pac.S)
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
                add_test(NAME checkpoint_entry_${mode}_pac_fpac
                    COMMAND ${entry_test} pac-fpac)
                set_tests_properties(checkpoint_entry_${mode}_pac_fpac PROPERTIES
                    TIMEOUT 30 SKIP_RETURN_CODE 77)
                if(mode STREQUAL "nested")
                    add_test(NAME checkpoint_entry_nested_bti_backend_live_chain
                        COMMAND ${entry_test} bti-backend-live-chain)
                    set_tests_properties(checkpoint_entry_nested_bti_backend_live_chain PROPERTIES
                        TIMEOUT 10 SKIP_RETURN_CODE 77)
                endif()
            endif()
            if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "mte")
                add_test(NAME checkpoint_entry_${mode}_memlog-mte
                    COMMAND ${entry_test} memlog-mte)
                set_tests_properties(checkpoint_entry_${mode}_memlog-mte PROPERTIES TIMEOUT 10)
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
        tests/checkpoint_x64_simd_normal_path.S tests/contract_module_record.c
        tests/coverage_provider.c ${CHECKPOINT_RUNTIME_SOURCES})
    teapot_configure_checkpoint_target(checkpoint_x64_simd_normal_path_test)
    target_compile_definitions(checkpoint_x64_simd_normal_path_test PRIVATE COVERAGE
        COVERAGE_PROVIDER_WITHOUT_TRACE_PC)
    target_compile_options(checkpoint_x64_simd_normal_path_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_simd_normal_path_test PRIVATE -no-pie)
    add_test(
        NAME checkpoint_x64_simd_normal_path
        COMMAND checkpoint_x64_simd_normal_path_test)

    add_executable(checkpoint_x64_vector_cases_test
        tests/checkpoint_x64_vector_cases.c tests/checkpoint_x64_vector_cases.S
        tests/contract_module_record.c tests/coverage_provider.c ${CHECKPOINT_RUNTIME_SOURCES})
    teapot_configure_checkpoint_target(checkpoint_x64_vector_cases_test)
    target_compile_definitions(checkpoint_x64_vector_cases_test PRIVATE COVERAGE ENABLE_NESTED_SPECULATION
        COVERAGE_PROVIDER_WITHOUT_TRACE_PC)
    target_compile_options(checkpoint_x64_vector_cases_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_vector_cases_test PRIVATE -no-pie)
    add_test(NAME checkpoint_x64_vector_cases COMMAND checkpoint_x64_vector_cases_test)
    set_tests_properties(checkpoint_x64_vector_cases PROPERTIES TIMEOUT 10)
endif()

if(BUILD_TESTING AND CHECKPOINT_ARCH_NAME STREQUAL "x64" AND TEAPOT_TEST_ASAN_AVAILABLE)
    add_executable(checkpoint_x64_xsave_report_test
        tests/checkpoint_x64_xsave_report.c
        tests/checkpoint_x64_xsave_report.S tests/contract_module_record.c)
    teapot_configure_checkpoint_target(checkpoint_x64_xsave_report_test)
    target_link_libraries(checkpoint_x64_xsave_report_test PRIVATE checkpoint)
    if(TEAPOT_ENABLE_COVERAGE)
        # Compiled in, so that it binds before ASan's own weak Sanitizer
        # Coverage callbacks.
        target_sources(checkpoint_x64_xsave_report_test PRIVATE tests/coverage_provider.c)
    endif()
    target_compile_options(checkpoint_x64_xsave_report_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_xsave_report_test PRIVATE -no-pie -fsanitize=address)
    target_link_options(checkpoint_x64_xsave_report_test PRIVATE
        -Wl,--wrap=report_gadget_x64_impl_KASPER_CACHE)
    add_test(
        NAME checkpoint_x64_xsave_report
        COMMAND checkpoint_x64_xsave_report_test)
    set_tests_properties(
        checkpoint_x64_xsave_report PROPERTIES SKIP_RETURN_CODE 77)
    if(TEAPOT_ENABLE_DIFT_RUNTIME)
        include(cmake/AsanLayoutProbe.cmake)
        teapot_check_asan_layout()
        # An incompatible mapping fails in preinit, before main can return 77.
        if(DEFINED TEAPOT_TEST_ASAN_LAYOUT_COMPATIBLE AND NOT TEAPOT_TEST_ASAN_LAYOUT_COMPATIBLE)
            message(WARNING "Disabling the XSAVE report test: linked ASan is incompatible with DIFT layout ${TEAPOT_DIFT_LAYOUT}; see the configure probe output")
            set_tests_properties(checkpoint_x64_xsave_report PROPERTIES DISABLED TRUE)
        endif()
    endif()
endif()

if(BUILD_TESTING AND CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
    add_executable(checkpoint_aarch64_report_call_nop_test
        tests/checkpoint_aarch64_report_call_nop.c src/report_gadget.c)
    teapot_configure_checkpoint_target(checkpoint_aarch64_report_call_nop_test)
    # Test the patcher without linking the full runtime's startup mappings.
    target_compile_options(checkpoint_aarch64_report_call_nop_test PRIVATE
        -ffunction-sections -fdata-sections -O0 -fno-builtin-memcpy)
    target_link_options(checkpoint_aarch64_report_call_nop_test PRIVATE
        -Wl,--gc-sections -Wl,--wrap=memcpy)
    add_test(NAME checkpoint_aarch64_report_call_nop
        COMMAND checkpoint_aarch64_report_call_nop_test)

    add_executable(checkpoint_aarch64_report_bti_test
        tests/checkpoint_aarch64_report_bti.c src/report_gadget.c)
    teapot_configure_checkpoint_target(checkpoint_aarch64_report_bti_test)
    # Test the patcher without linking the full runtime's startup mappings.
    target_compile_options(checkpoint_aarch64_report_bti_test PRIVATE
        -ffunction-sections -fdata-sections -O0 -fno-builtin-memcpy)
    target_link_options(checkpoint_aarch64_report_bti_test PRIVATE
        -Wl,--gc-sections)
    add_test(NAME checkpoint_aarch64_report_bti
        COMMAND checkpoint_aarch64_report_bti_test)
    set_tests_properties(checkpoint_aarch64_report_bti PROPERTIES
        TIMEOUT 10 SKIP_RETURN_CODE 77)
endif()

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN "aarch64_shadow_stack.h" EXCLUDE)

# The guarded-copy linker script of the aarch64-bti-pac target. Software mode
# needs no layout: its indirect-target check tests only the marker pair.
install(FILES
    cmake/AArch64Bti.ld
    DESTINATION "${CMAKE_INSTALL_DATADIR}/libcheckpoint/cmake")
