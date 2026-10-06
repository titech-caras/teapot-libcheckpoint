# Persistent registrations for the reviewed v4 executable/template fixtures.
# These are test-only targets, not runtime archive sources or installed exports.
add_library(checkpoint_fault_risc_template_bridge_lib SHARED tests/fault_risc_template_bridge.c)
target_include_directories(checkpoint_fault_risc_template_bridge_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(checkpoint_fault_risc_template_bridge_lib PRIVATE -UNDEBUG -fno-stack-protector)
add_executable(checkpoint_fault_risc_template_bridge_test tests/fault_risc_template_bridge_test.c)
target_include_directories(checkpoint_fault_risc_template_bridge_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(checkpoint_fault_risc_template_bridge_test PRIVATE -UNDEBUG)
target_link_libraries(checkpoint_fault_risc_template_bridge_test PRIVATE checkpoint_fault_risc_template_bridge_lib)
add_test(NAME checkpoint_fault_risc_template_bridge COMMAND checkpoint_fault_risc_template_bridge_test)
set_tests_properties(checkpoint_fault_risc_template_bridge PROPERTIES TIMEOUT 30 LABELS "fault-risc;template")

if(NOT TEAPOT_ENABLE_FAULT_PUBLISHING OR NOT CHECKPOINT_ARCH_NAME MATCHES "^(aarch64|riscv64)$")
    return()
endif()

# Exact retained emitter/printer output. Python discovery regenerates both
# fixtures and compares all text after canonicalizing only generated UUIDs.
add_library(checkpoint_fault_risc_fixture OBJECT "tests/fixtures/fault-risc/${CHECKPOINT_ARCH_NAME}.S")
if(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
    # Required only for this checked-in v4 fixture. Never a runtime/global flag.
    target_compile_options(checkpoint_fault_risc_fixture PRIVATE -Wa,--no-pad-sections)
endif()

function(teapot_fault_risc_executable target source)
    add_executable(${target} ${source} tests/fault_risc_execution.S
        $<TARGET_OBJECTS:checkpoint_fault_risc_fixture>)
    teapot_configure_checkpoint_target(${target})
    target_compile_options(${target} PRIVATE -O2 -fno-pie)
    target_link_options(${target} PRIVATE -no-pie -Wl,-z,separate-code,--build-id=none)
endfunction()

teapot_fault_risc_executable(checkpoint_fault_risc_execution_test tests/fault_risc_execution.c)
target_link_libraries(checkpoint_fault_risc_execution_test PRIVATE checkpoint)
foreach(mode IN ITEMS off on)
    add_test(NAME checkpoint_fault_risc_execution_${mode} COMMAND checkpoint_fault_risc_execution_test ${mode})
    set_tests_properties(checkpoint_fault_risc_execution_${mode} PROPERTIES TIMEOUT 30 LABELS "fault-risc;execution")
endforeach()

if(CHECKPOINT_ARCH_NAME STREQUAL "aarch64" AND TEAPOT_EXPERIMENTAL_AARCH64_BTI)
    teapot_fault_risc_executable(checkpoint_fault_risc_bti_pac_test tests/fault_risc_execution.c)
    target_compile_definitions(checkpoint_fault_risc_bti_pac_test PRIVATE FAULT_RISC_TEST_BTI_PAC)
    target_link_libraries(checkpoint_fault_risc_bti_pac_test PRIVATE checkpoint)
    foreach(mode IN ITEMS off on)
        add_test(NAME checkpoint_fault_risc_bti_pac_${mode} COMMAND checkpoint_fault_risc_bti_pac_test ${mode})
        set_tests_properties(checkpoint_fault_risc_bti_pac_${mode} PROPERTIES TIMEOUT 30 LABELS "fault-risc;bti-pac")
    endforeach()
endif()

if(TARGET checkpoint_nested)
    teapot_fault_risc_executable(checkpoint_fault_risc_rollback_test tests/fault_risc_rollback.c)
    target_sources(checkpoint_fault_risc_rollback_test PRIVATE tests/checkpoint_entry.S)
    target_compile_definitions(checkpoint_fault_risc_rollback_test PRIVATE ENABLE_NESTED_SPECULATION)
    if(TEAPOT_ENABLE_COVERAGE)
        target_sources(checkpoint_fault_risc_rollback_test PRIVATE tests/coverage_provider.c)
    endif()
    target_link_libraries(checkpoint_fault_risc_rollback_test PRIVATE checkpoint_nested m dl)
    set(fault_risc_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
    if(CHECKPOINT_ARCH_NAME STREQUAL "aarch64" AND fault_risc_emulator)
        list(GET fault_risc_emulator 0 fault_risc_emulator_program)
        if(fault_risc_emulator_program MATCHES "(^|/)qemu-aarch64$")
            # The existing A64 runtime needs both 8-MiB stack halves. This is
            # the retained fixture's QEMU stack argument, not a product change.
            list(INSERT fault_risc_emulator 1 -s 16777216)
        endif()
    endif()
    foreach(nested IN ITEMS 0 1)
        foreach(replay IN ITEMS 0 1)
            foreach(mapped IN ITEMS 0 1)
                set(test checkpoint_fault_risc_rollback_${nested}_${replay}_${mapped})
                add_test(NAME ${test} COMMAND "${CMAKE_COMMAND}"
                    "-DPROGRAM=$<TARGET_FILE:checkpoint_fault_risc_rollback_test>"
                    "-DEMULATOR=${fault_risc_emulator}"
                    "-DNESTED=${nested}" "-DREPLAY=${replay}" "-DMAPPED=${mapped}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fault_risc_compare.cmake")
                set_tests_properties(${test} PROPERTIES TIMEOUT 60 LABELS "fault-risc;rollback;on-off-parity")
            endforeach()
        endforeach()
    endforeach()
endif()
