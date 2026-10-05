# The coverage mode is part of the contract ABI (runtime_contract.h): the
# generated MANIFEST must state it as the build was configured (ENABLED), both
# as the fingerprinted ABI key and as the capability.
file(READ "${MANIFEST}" manifest)
if(ENABLED)
    set(abi 1)
    set(capability true)
else()
    set(abi 0)
    set(capability false)
endif()
# Within the "abi" section the key holds a number, within "capabilities" a
# boolean; each appears once.
string(REGEX MATCHALL "\"coverage\": [0-9]+" abi_values "${manifest}")
string(REGEX MATCHALL "\"coverage\": (true|false)" capability_values "${manifest}")
if(NOT abi_values STREQUAL "\"coverage\": ${abi}")
    message(FATAL_ERROR "${MANIFEST}: ABI coverage is ${abi_values}, expected ${abi}")
endif()
if(NOT capability_values STREQUAL "\"coverage\": ${capability}")
    message(FATAL_ERROR "${MANIFEST}: the coverage capability is ${capability_values}, expected ${capability}")
endif()
message(STATUS "${MANIFEST}: coverage ${abi}")
