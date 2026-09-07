cmake_minimum_required(VERSION 3.16)

project(libcheckpoint C ASM)

include(CTest)

include(GNUInstallDirs)
include(cmake/Options.cmake)
include(cmake/TargetArchitecture.cmake)
include(cmake/DiftLayout.cmake)
include(cmake/RuntimeTargets.cmake)
include(cmake/DiftWrappers.cmake)

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

    add_executable(checkpoint_x64_xsave_report_test
        tests/checkpoint_x64_xsave_report.c
        tests/checkpoint_x64_xsave_report.S)
    teapot_configure_checkpoint_target(checkpoint_x64_xsave_report_test)
    target_link_libraries(checkpoint_x64_xsave_report_test PRIVATE checkpoint)
    target_compile_options(checkpoint_x64_xsave_report_test PRIVATE -fno-pie)
    target_link_options(checkpoint_x64_xsave_report_test PRIVATE -no-pie)
    add_test(
        NAME checkpoint_x64_xsave_report
        COMMAND checkpoint_x64_xsave_report_test)
    set_tests_properties(
        checkpoint_x64_xsave_report PROPERTIES SKIP_RETURN_CODE 77)
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

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
