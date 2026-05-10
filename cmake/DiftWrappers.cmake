function(teapot_configure_dift_wrapper_target target)
    target_include_directories(${target} PUBLIC
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
    find_package(ZLIB)
    if(ZLIB_FOUND)
        add_library(checkpoint_dift_zlib_wrappers src/dift_wrappers/dift_zlib_wrappers.c)
        teapot_configure_dift_wrapper_target(checkpoint_dift_zlib_wrappers)
        target_link_libraries(checkpoint_dift_zlib_wrappers PUBLIC ZLIB::ZLIB)
        install(TARGETS checkpoint_dift_zlib_wrappers ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    else()
        message(STATUS "zlib not found; skipping optional DIFT zlib wrappers")
    endif()
endif()
