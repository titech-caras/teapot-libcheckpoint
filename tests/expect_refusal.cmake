# Pass when PROGRAM (run through EMULATOR when cross testing) is refused at
# start-up: it must not exit normally, its output must match EXPECTED, and none
# of its constructors may have run. CTest itself counts a signal as a failure
# whatever the output, hence this script.
execute_process(COMMAND ${EMULATOR} "${PROGRAM}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
set(output "${out}${err}")
message("${output}")
if(result EQUAL 0)
    message(FATAL_ERROR "the program started (exit 0)")
endif()
if(NOT output MATCHES "${EXPECTED}")
    message(FATAL_ERROR "the refusal does not say: ${EXPECTED} (result ${result})")
endif()
if(output MATCHES "instrumented constructor ran|contract accepted")
    message(FATAL_ERROR "the program ran before the refusal")
endif()
