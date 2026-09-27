function(teapot_configure_dift_wrapper_target target)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_compile_options(${target} PRIVATE -fno-stack-protector)
    target_compile_definitions(${target} PRIVATE
        DIFT_XOR_MASK=${DIFT_XOR_MASK}
        ${DIFT_RANGE_DEFS})
endfunction()

if(TEAPOT_BUILD_DIFT_MATH_WRAPPERS)
    add_library(checkpoint_dift_math_wrappers src/dift_wrappers/dift_math_wrappers.c)
    teapot_configure_dift_wrapper_target(checkpoint_dift_math_wrappers)
    target_link_libraries(checkpoint_dift_math_wrappers PUBLIC m)
    install(TARGETS checkpoint_dift_math_wrappers ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

if(TEAPOT_BUILD_DIFT_ZLIB_WRAPPERS)
    find_package(ZLIB 1.2.6 REQUIRED)
    if(ZLIB_FOUND)
        add_library(checkpoint_dift_zlib_wrappers src/dift_wrappers/dift_zlib_wrappers.c)
        teapot_configure_dift_wrapper_target(checkpoint_dift_zlib_wrappers)
        target_link_libraries(checkpoint_dift_zlib_wrappers PUBLIC ZLIB::ZLIB)
        install(TARGETS checkpoint_dift_zlib_wrappers ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
        if(BUILD_TESTING)
            add_executable(checkpoint_dift_zlib_no_output_test tests/dift_zlib_no_output.c)
            teapot_configure_dift_wrapper_target(checkpoint_dift_zlib_no_output_test)
            target_link_libraries(checkpoint_dift_zlib_no_output_test PRIVATE checkpoint_dift_zlib_wrappers)
            add_test(NAME checkpoint_dift_zlib_no_output COMMAND checkpoint_dift_zlib_no_output_test)

            add_executable(checkpoint_dift_zlib_stream_test
                tests/dift_zlib_stream.c src/dift_wrappers/dift_zlib_wrappers.c)
            target_include_directories(checkpoint_dift_zlib_stream_test PRIVATE
                "${CMAKE_CURRENT_BINARY_DIR}/include" include)
            target_compile_definitions(checkpoint_dift_zlib_stream_test PRIVATE DIFT_XOR_MASK=0x70000000ULL)
            target_link_libraries(checkpoint_dift_zlib_stream_test PRIVATE ZLIB::ZLIB)
            target_link_options(checkpoint_dift_zlib_stream_test PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free)
            foreach(check IN ITEMS range streaming lifetime independent raw-checksum dictionary errors missing
                                   boundaries allocation-failure)
                add_test(NAME checkpoint_dift_zlib_${check} COMMAND checkpoint_dift_zlib_stream_test ${check})
                set_tests_properties(checkpoint_dift_zlib_${check} PROPERTIES TIMEOUT 10)
            endforeach()
        endif()
    endif()
endif()
