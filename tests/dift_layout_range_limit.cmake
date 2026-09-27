include("${CMAKE_CURRENT_LIST_DIR}/../cmake/DiftLayoutValidation.cmake")

set(ranges 0x1000:0x2000 0x3000:0x4000 0x5000:0x6000
           0x7000:0x8000 0x9000:0xa000)
if(TEST_SIX_RANGES)
    list(APPEND ranges 0xb000:0xc000)
endif()
teapot_validate_dift_layout(range-limit 0x10000000 0x1000000 ${ranges})

execute_process(COMMAND "${CMAKE_COMMAND}" -DTEST_SIX_RANGES=ON
    -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "runtime supports at most 5")
    message(FATAL_ERROR "Six-range layout was not rejected: ${status}; ${output}; ${error}")
endif()
