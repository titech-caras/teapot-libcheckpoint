# Every writable runtime global lives in teapot_protected or teapot_protected_bss,
# so that a speculative wild store hits ASan poison instead of runtime state.
# Usage: cmake -DNM=<nm> -DLIBRARY=<archive> [-DRUNTIME=ON] -P check_protected_globals.cmake
# An empty listing fails. RUNTIME=ON also requires two known protected objects,
# so that a listing this script cannot read (another nm format, an LTO archive)
# fails instead of passing.
#
# Globals that stay outside, each touched only before or outside simulation:
set(ALLOWED
    runtime_mapped_ranges        # shadow mappings recorded at start-up
    runtime_mapped_range_count
    signal_stack                 # the sigaltstack descriptor, handed to the kernel once
    probe_target                 # the BTI activation probe's target
)
# Data, BSS, small data, common, weak and unique objects. nm classes every weak
# object V whatever its section, so the section decides for those. Read-only
# sections (.rodata, RISC-V's .srodata, and .data.rel.ro once relocated) and the
# start-up pointer arrays cannot hold mutable runtime state.
set(WRITABLE_CLASSES "^[DdBbSsGgCVu]$")
set(EXCLUDED_SECTIONS "^(teapot_protected|\\.s?rodata|\\.data\\.rel\\.ro|\\.(pre)?init_array|\\.fini_array)")
execute_process(COMMAND "${NM}" --defined-only --format=sysv "${LIBRARY}"
                OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${LIBRARY}")
endif()
string(REPLACE "\n" ";" lines "${symbols}")
set(unprotected)
set(seen)
set(controls)
set(rows 0)
foreach(line IN LISTS lines)
    string(REPLACE "|" ";" fields "${line}")
    list(LENGTH fields count)
    if(count LESS 7)
        continue()
    endif()
    math(EXPR rows "${rows} + 1")
    list(GET fields 0 name)
    list(GET fields 2 class)
    list(GET fields 6 section)
    string(STRIP "${name}" name)
    string(STRIP "${class}" class)
    string(STRIP "${section}" section)
    if("${name}|${section}" STREQUAL "checkpoint_cnt|teapot_protected" OR
       "${name}|${section}" STREQUAL "scratchpad|teapot_protected_bss")
        list(APPEND controls "${name}")
    endif()
    if(NOT class MATCHES "${WRITABLE_CLASSES}" OR section MATCHES "${EXCLUDED_SECTIONS}")
        continue()
    endif()
    list(FIND ALLOWED "${name}" allowed)
    if(allowed EQUAL -1)
        list(APPEND unprotected "${name} (${section})")
    else()
        list(APPEND seen "${name}")
    endif()
endforeach()
if(rows EQUAL 0)
    message(FATAL_ERROR "${LIBRARY}: no symbol rows in the output of ${NM}")
endif()
if(unprotected)
    list(JOIN unprotected ", " text)
    message(FATAL_ERROR "writable runtime globals outside teapot_protected: ${text}")
endif()
if(RUNTIME)
    list(REMOVE_DUPLICATES controls)
    list(LENGTH controls found)
    if(NOT found EQUAL 2)
        message(FATAL_ERROR "${LIBRARY}: expected checkpoint_cnt in teapot_protected and scratchpad in "
                            "teapot_protected_bss; found only '${controls}'")
    endif()
endif()
if(RUNTIME)
    set(unused)
    foreach(name IN LISTS ALLOWED)
        list(FIND seen "${name}" index)
        if(index EQUAL -1)
            list(APPEND unused "${name}")
        endif()
    endforeach()
    if(unused)
        list(JOIN unused ", " text)
        message(STATUS "allowed but not defined here: ${text}")
    endif()
endif()
message(STATUS "every writable runtime global is protected or allowed")
