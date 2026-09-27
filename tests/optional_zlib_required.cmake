if(DEFINED TEAPOT_BUILD_DIFT_ZLIB_WRAPPERS)
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB ON)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/DiftWrappers.cmake")
    return()
endif()

foreach(requested IN ITEMS OFF ON)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DTEAPOT_BUILD_DIFT_ZLIB_WRAPPERS=${requested}" -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(requested)
        if(status EQUAL 0 OR NOT error MATCHES "[Rr][Ee][Qq][Uu][Ii][Rr][Ee][Dd]")
            message(FATAL_ERROR "Explicit zlib request did not fail: ${status}; ${output}; ${error}")
        endif()
    elseif(NOT status EQUAL 0)
        message(FATAL_ERROR "Disabled wrappers required zlib: ${status}; ${output}; ${error}")
    endif()
endforeach()
