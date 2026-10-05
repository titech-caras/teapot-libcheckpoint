# The rewrite/runtime contract (include/runtime_contract.h).
#
# A probe compiled with the target compiler, never run, so cross builds work,
# turns each fact into a string of digits, as CMake's CheckTypeSize does, and
# file(STRINGS) reads them back. The facts thus come from the real headers,
# sizeof and offsetof, not from a second copy. Outputs:
#   contract/runtime_contract_record.S  the runtime record, in every archive
#   include/runtime_contract_fingerprint.h  the fingerprint and anchor name
#   lib<archive>.contract.json  per archive, next to it and installed beside it

set(LIBCHECKPOINT_CONTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/contract")
file(MAKE_DIRECTORY "${LIBCHECKPOINT_CONTRACT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/include")
# The probe runs when CMake configures, so editing a probed header must
# configure again; otherwise the build would keep a stale contract.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/checkpoint.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/dift_support.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime_contract.h"
    "${TEAPOT_AARCH64_SHADOW_STACK_CONFIG}")

# The ABI: what Teapot's emitted code and the runtime both depend on, as
# KEY|C-EXPRESSION. Teapot compares every key, so add a key only together with
# Teapot's side, and never runtime-only facts (see the runtime section below).
set(_contract_abi
    "contract.version|LIBCHECKPOINT_CONTRACT_VERSION"
    "word_size|sizeof(void *)"
    "little_endian|(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)"
    "scratchpad.size|SCRATCHPAD_SIZE"
    "scratchpad.alignment|__alignof__(scratchpad_t)"
    "scratchpad.first_spill|SCRATCHPAD_FIRST_SPILL_OFFSET"
    "memlog.entry_size|sizeof(memory_history_t)"
    "memlog.addr_offset|offsetof(memory_history_t, addr)"
    "memlog.data_offset|offsetof(memory_history_t, data)"
    "memlog.data_width|sizeof(((memory_history_t *)0)->data)"
    "memlog.size_offset|offsetof(memory_history_t, size)"
    "memlog.size_width|sizeof(((memory_history_t *)0)->size)"
    "target_metadata.trampoline|CHECKPOINT_TARGET_TRAMPOLINE_ADDR"
    "target_metadata.return|CHECKPOINT_TARGET_RETURN_ADDR"
    "target_metadata.branch_counter|CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR"
    "branch_counter.width|CHECKPOINT_BRANCH_COUNTER_SIZE"
    "counters.instruction_cnt_width|sizeof(instruction_cnt)"
    "counters.checkpoint_cnt_width|sizeof(checkpoint_cnt)"
    "guards.list_entry_width|sizeof(guard_list[0])"
    # Whether a rollback replays the speculative coverage guards into a fuzzer
    # (COVERAGE, from TEAPOT_ENABLE_COVERAGE). Teapot emits the guard pushes for
    # exactly such a runtime, so the mode belongs to the fingerprint: an archive
    # built the other way has another anchor and refuses the module.
    "coverage|LCK_COVERAGE"
    "dift.xor_mask|DIFT_XOR_MASK"
    "dift.asan_shadow_offset|LCK_DIFT_ASAN_SHADOW_OFFSET"
    "dift.reg_tags_size|DIFT_REG_TAGS_SIZE"
    "dift.tag_size|sizeof(dift_tag_t)"
    "dift.reg_tags_bytes|sizeof(dift_reg_tags)"
    "dift.reg_tags_alignment|__alignof__(dift_reg_tags)"
    "dift.queued_tags_alignment|__alignof__(dift_reg_queued_tags)"
    "dift.queue_pending_size|sizeof(dift_reg_queue_pending)"
    "dift.tag.attacker|TAG_ATTACKER"
    "dift.tag.attacker_indirect|TAG_ATTACKER_INDIRECT"
    "dift.tag.secret|TAG_SECRET"
    "dift.tag.secret_indirect|TAG_SECRET_INDIRECT"
    "dift.arg0|DIFT_ARG0"
    "dift.arg1|DIFT_ARG1"
    "dift.arg2|DIFT_ARG2"
    "dift.arg3|DIFT_ARG3"
    "dift.arg4|DIFT_ARG4"
    "dift.arg5|DIFT_ARG5"
    "dift.ret|DIFT_RET")

if(CHECKPOINT_ARCH_NAME STREQUAL "x64")
    foreach(reg IN ITEMS rax rbx rcx rdx rsi rdi rsp rbp r8 r9 r10 r11 r12 r13 r14 r15)
        string(TOUPPER "${reg}" upper)
        list(APPEND _contract_abi "dift.reg.${reg}|DIFT_REG_${upper}")
    endforeach()
    list(APPEND _contract_abi
        "report.x64.tag_spill|X64_REPORT_TAG_OFFSET"
        "report.x64.call_stack|X64_REPORT_CALL_STACK_OFFSET"
        "x64.vector_state.xmm0-7|LIBCHECKPOINT_X64_VECTOR_XMM0_7"
        "x64.vector_state.sse|LIBCHECKPOINT_X64_VECTOR_SSE"
        "x64.vector_state.avx|LIBCHECKPOINT_X64_VECTOR_AVX"
        "x64.vector_state.full|LIBCHECKPOINT_X64_VECTOR_FULL")
elseif(CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
    foreach(index RANGE 0 30)
        list(APPEND _contract_abi
            "dift.reg.x${index}|DIFT_REG_X${index}"
            "checkpoint.reg.x${index}|offsetof(checkpoint_register_state_t, x${index})")
    endforeach()
    list(APPEND _contract_abi
        "dift.reg.sp|DIFT_REG_SP"
        "target_metadata.scratch_reg|CHECKPOINT_TARGET_SCRATCH_REG_ADDR"
        "target_metadata.fixed_reg0_source|CHECKPOINT_TARGET_FIXED_REG0_SOURCE"
        "target_metadata.fixed_reg1_source|CHECKPOINT_TARGET_FIXED_REG1_SOURCE"
        "target_metadata.fixed_reg_none|CHECKPOINT_TARGET_FIXED_REG_NONE"
        "report.aarch64.state|AARCH64_REPORT_STATE_OFFSET"
        "report.aarch64.gadget_addr|AARCH64_REPORT_GADGET_ADDR"
        "report.aarch64.access_addr|AARCH64_REPORT_ACCESS_ADDR"
        "report.aarch64.tag|AARCH64_REPORT_TAG"
        "report.aarch64.runtime_save|AARCH64_REPORT_RUNTIME_SAVE"
        "report.aarch64.simd_state|AARCH64_REPORT_SIMD_STATE_OFFSET"
        "report.aarch64.call_stack|AARCH64_REPORT_CALL_STACK_OFFSET")
    foreach(slot IN ITEMS SIZE DIFT_OFFSET MEMLOG_OFFSET ASAN_OFFSET GADGET_MEM_OFFSET
                          GADGET_PORT_OFFSET COVERAGE_OFFSET CONTROL_OFFSET RESTORE_OFFSET
                          INDIRECT_TARGET_OFFSET REPORT_OFFSET ABI_OFFSET
                          TEXT_DIFT_CAPTURE_OFFSET TEXT_DIFT_LLVM_OFFSET)
        string(TOLOWER "${slot}" key)
        list(APPEND _contract_abi "aarch64.shadow_stack.${key}|AARCH64_SHADOW_STACK_${slot}")
    endforeach()
elseif(CHECKPOINT_ARCH_NAME STREQUAL "riscv64")
    foreach(reg IN ITEMS zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7
                         s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6)
        string(TOUPPER "${reg}" upper)
        list(APPEND _contract_abi "dift.reg.${reg}|DIFT_REG_${upper}")
    endforeach()
    foreach(index RANGE 1 31)
        list(APPEND _contract_abi
            "checkpoint.reg.x${index}|offsetof(checkpoint_register_state_t, x${index})")
    endforeach()
    list(APPEND _contract_abi
        "scratchpad.riscv64_original_tp|RISCV64_ORIGINAL_TP_OFFSET"
        "target_metadata.scratch_reg|CHECKPOINT_TARGET_SCRATCH_REG_ADDR"
        "target_metadata.fixed_reg0_source|CHECKPOINT_TARGET_FIXED_REG0_SOURCE"
        "target_metadata.fixed_reg1_source|CHECKPOINT_TARGET_FIXED_REG1_SOURCE"
        "target_metadata.fixed_reg_none|CHECKPOINT_TARGET_FIXED_REG_NONE")
endif()

# The application ranges the DIFT shadow covers. Teapot takes them from the
# contract and does not emit them, but a narrower mapping would leave an
# address the rewrite was told is covered without a shadow, so they are part
# of the ABI and of the fingerprint: the archive linked must cover exactly
# what the rewrite was checked against. Unused slots are zero.
list(APPEND _contract_abi "dift.app_range_count|LCK_DIFT_APP_RANGE_COUNT")
foreach(index RANGE 0 4)
    list(APPEND _contract_abi
        "dift.app_range${index}.start|LCK_DIFT_APP_RANGE${index}_START"
        "dift.app_range${index}.end|LCK_DIFT_APP_RANGE${index}_END")
endforeach()

# The runtime's own facts and build switches: recorded, never compared.
set(_contract_runtime
    "capabilities|LIBCHECKPOINT_RUNTIME_CAPABILITIES"
    "max_checkpoints|MAX_CHECKPOINTS"
    "mem_history_len|MEM_HISTORY_LEN"
    "guard_list_len|GUARD_LIST_LEN"
    "checkpoint_metadata_size|CHECKPOINT_METADATA_SIZE"
    "target_metadata_size|CHECKPOINT_TARGET_METADATA_SIZE"
    "verbose|LCK_VERBOSE"
    "time|LCK_TIME"
    "branch_exec_count|LCK_BRANCH_EXEC_COUNT")

# In the bit order of runtime_contract.h.
set(_contract_capability_names
    nested aarch64_bti_pac dift_runtime x64_vector_full coverage riscv64_float_state
    x64_vector_sse x64_vector_avx)

function(_libcheckpoint_contract_probe_source out)
    set(source "/* Generated by cmake/RuntimeContract.cmake. */
#include <stddef.h>
#include <stdint.h>
#include \"checkpoint.h\"
#include \"runtime_contract.h\"

#ifdef VERBOSE
#define LCK_VERBOSE 1
#else
#define LCK_VERBOSE 0
#endif
#ifdef TIME
#define LCK_TIME 1
#else
#define LCK_TIME 0
#endif
#ifdef USE_BRANCH_EXEC_COUNT
#define LCK_BRANCH_EXEC_COUNT 1
#else
#define LCK_BRANCH_EXEC_COUNT 0
#endif
#ifdef COVERAGE
#define LCK_COVERAGE 1
#else
#define LCK_COVERAGE 0
#endif
")
    set(count 0)
    foreach(index RANGE 0 4)
        string(APPEND source "#ifdef DIFT_APP_RANGE${index}_START
#define LCK_DIFT_APP_RANGE${index}_START DIFT_APP_RANGE${index}_START
#define LCK_DIFT_APP_RANGE${index}_END DIFT_APP_RANGE${index}_END
#define LCK_DIFT_APP_RANGE_COUNT_${index} 1
#else
#define LCK_DIFT_APP_RANGE${index}_START 0
#define LCK_DIFT_APP_RANGE${index}_END 0
#define LCK_DIFT_APP_RANGE_COUNT_${index} 0
#endif
")
    endforeach()
    string(APPEND source "#define LCK_DIFT_APP_RANGE_COUNT (LCK_DIFT_APP_RANGE_COUNT_0 + LCK_DIFT_APP_RANGE_COUNT_1 + \\
    LCK_DIFT_APP_RANGE_COUNT_2 + LCK_DIFT_APP_RANGE_COUNT_3 + LCK_DIFT_APP_RANGE_COUNT_4)

#define LCK_NEG(v) ((v) < 0)
#define LCK_ABS(v) (LCK_NEG(v) ? 0ULL - (unsigned long long)(v) : (unsigned long long)(v))
#define LCK_D(v, p) ((char)('0' + (char)((LCK_ABS(v) / (p)) % 10ULL)))
#define LCK_DIGITS(v) \\
    LCK_D(v, 10000000000000000000ULL), LCK_D(v, 1000000000000000000ULL), \\
    LCK_D(v, 100000000000000000ULL), LCK_D(v, 10000000000000000ULL), \\
    LCK_D(v, 1000000000000000ULL), LCK_D(v, 100000000000000ULL), \\
    LCK_D(v, 10000000000000ULL), LCK_D(v, 1000000000000ULL), \\
    LCK_D(v, 100000000000ULL), LCK_D(v, 10000000000ULL), LCK_D(v, 1000000000ULL), \\
    LCK_D(v, 100000000ULL), LCK_D(v, 10000000ULL), LCK_D(v, 1000000ULL), \\
    LCK_D(v, 100000ULL), LCK_D(v, 10000ULL), LCK_D(v, 1000ULL), LCK_D(v, 100ULL), \\
    LCK_D(v, 10ULL), LCK_D(v, 1ULL)
#define LCK_ENTRY(a, b, c, v) \\
    {'L', 'C', 'K', '[', a, b, c, ']', LCK_NEG(v) ? '-' : '+', LCK_DIGITS(v), ';', 0}
")
    set(index 0)
    foreach(field IN LISTS ARGN)
        string(REPLACE "|" ";" parts "${field}")
        list(GET parts 1 expression)
        math(EXPR hundreds "${index} / 100")
        math(EXPR tens "(${index} / 10) % 10")
        math(EXPR ones "${index} % 10")
        string(APPEND source
            "const char lck_${index}[] = LCK_ENTRY('${hundreds}', '${tens}', '${ones}', (${expression}));\n")
        math(EXPR index "${index} + 1")
    endforeach()
    set(${out} "${source}" PARENT_SCOPE)
endfunction()

# Compile the probe for one archive configuration; set OUT_PREFIX_<key> to each
# value, in decimal.
function(_libcheckpoint_contract_probe name out_prefix)
    set(fields ${_contract_abi} ${_contract_runtime})
    _libcheckpoint_contract_probe_source(source ${fields})
    set(probe_dir "${LIBCHECKPOINT_CONTRACT_DIR}/probe-${name}")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/probe.c" "${source}")

    set(definitions "-DDIFT_XOR_MASK=${DIFT_XOR_MASK}"
        "-DLCK_DIFT_ASAN_SHADOW_OFFSET=${DIFT_ASAN_SHADOW_OFFSET}")
    foreach(definition IN LISTS TEAPOT_DIFT_RUNTIME_DEFS TEAPOT_ARCH_RUNTIME_DEFS DIFT_RANGE_DEFS ARGN)
        list(APPEND definitions "-D${definition}")
    endforeach()
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(compiled "${probe_dir}/build" SOURCES "${probe_dir}/probe.c"
        COMPILE_DEFINITIONS ${definitions} -fno-lto -w
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR}/include"
        OUTPUT_VARIABLE output
        COPY_FILE "${probe_dir}/probe.a")
    if(NOT compiled)
        message(FATAL_ERROR "The runtime contract probe (${name}) does not compile:\n${output}")
    endif()
    file(STRINGS "${probe_dir}/probe.a" entries REGEX "LCK\\[[0-9][0-9][0-9]\\][-+][0-9]+;")
    set(index 0)
    foreach(field IN LISTS fields)
        string(REPLACE "|" ";" parts "${field}")
        list(GET parts 0 key)
        math(EXPR hundreds "${index} / 100")
        math(EXPR tens "(${index} / 10) % 10")
        math(EXPR ones "${index} % 10")
        set(match)
        foreach(entry IN LISTS entries)
            if(entry MATCHES "LCK\\[${hundreds}${tens}${ones}\\]([-+])([0-9]+);")
                set(match "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
            endif()
        endforeach()
        if(NOT match)
            message(FATAL_ERROR "The runtime contract probe (${name}) has no value for ${key}")
        endif()
        # Strip leading zeroes; math() cannot hold values above 2^63.
        string(REGEX REPLACE "^([-+])0*([0-9])" "\\1\\2" match "${match}")
        string(REGEX REPLACE "^\\+" "" match "${match}")
        if(match STREQUAL "-0")
            set(match 0)
        endif()
        set(${out_prefix}_${key} "${match}" PARENT_SCOPE)
        math(EXPR index "${index} + 1")
    endforeach()
endfunction()

foreach(value IN ITEMS "${CHECKPOINT_ARCH_NAME}" "${TEAPOT_DIFT_LAYOUT}" "${TEAPOT_AARCH64_TAG_STORAGE}")
    if(NOT value MATCHES "^[A-Za-z0-9_.:+-]+$")
        message(FATAL_ERROR "Cannot record ${value} in the runtime contract")
    endif()
endforeach()
set(_contract_tag_storage "${TEAPOT_AARCH64_TAG_STORAGE}")
if(NOT CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
    set(_contract_tag_storage "shadow")
endif()

_libcheckpoint_contract_probe(default _contract_default)
_libcheckpoint_contract_probe(nested _contract_nested ENABLE_NESTED_SPECULATION)

# The ABI section, flat and sorted. The fingerprint hashes KEY=VALUE lines.
set(_abi_keys isa dift.layout tag_storage)
set(_contract_default_isa "${CHECKPOINT_ARCH_NAME}")
set(_contract_default_dift.layout "${TEAPOT_DIFT_LAYOUT}")
set(_contract_default_tag_storage "${_contract_tag_storage}")
foreach(field IN LISTS _contract_abi)
    string(REPLACE "|" ";" parts "${field}")
    list(GET parts 0 key)
    list(APPEND _abi_keys "${key}")
    if(NOT "${_contract_default_${key}}" STREQUAL "${_contract_nested_${key}}")
        message(FATAL_ERROR "The nested runtime changes the contract ABI field ${key}")
    endif()
endforeach()
list(SORT _abi_keys)
set(_abi_lines "")
set(_abi_json "")
foreach(key IN LISTS _abi_keys)
    set(value "${_contract_default_${key}}")
    string(APPEND _abi_lines "${key}=${value}\n")
    if(value MATCHES "^-?[0-9]+$")
        list(APPEND _abi_json "    \"${key}\": ${value}")
    else()
        list(APPEND _abi_json "    \"${key}\": \"${value}\"")
    endif()
endforeach()
string(SHA256 _abi_hash "${_abi_lines}")
string(SUBSTRING "${_abi_hash}" 0 16 LIBCHECKPOINT_CONTRACT_FINGERPRINT)
set(_contract_version "${_contract_default_contract.version}")
set(LIBCHECKPOINT_CONTRACT_ANCHOR
    "__libcheckpoint_contract_v${_contract_version}_${LIBCHECKPOINT_CONTRACT_FINGERPRINT}")
string(REPLACE ";" ",\n" _abi_json "${_abi_json}")
message(STATUS "Runtime contract v${_contract_version}: ${LIBCHECKPOINT_CONTRACT_FINGERPRINT}")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/include/runtime_contract_fingerprint.h"
"/* Generated by cmake/RuntimeContract.cmake: this build's contract. */
#pragma once
#ifdef __ASSEMBLER__
#define LIBCHECKPOINT_CONTRACT_FINGERPRINT 0x${LIBCHECKPOINT_CONTRACT_FINGERPRINT}
#else
#define LIBCHECKPOINT_CONTRACT_FINGERPRINT 0x${LIBCHECKPOINT_CONTRACT_FINGERPRINT}ULL
#endif
#define LIBCHECKPOINT_CONTRACT_ANCHOR ${LIBCHECKPOINT_CONTRACT_ANCHOR}
")

# The runtime record embeds the ABI section; capability bits come from the
# options each archive is compiled with (LIBCHECKPOINT_RUNTIME_CAPABILITIES).
set(_record_json "{
  \"schema\": \"libcheckpoint-runtime-contract\",
  \"version\": ${_contract_version},
  \"fingerprint\": \"${LIBCHECKPOINT_CONTRACT_FINGERPRINT}\",
  \"abi\": {
${_abi_json}
  }
}
")
file(WRITE "${LIBCHECKPOINT_CONTRACT_DIR}/record.json" "${_record_json}")
file(READ "${LIBCHECKPOINT_CONTRACT_DIR}/record.json" _record_hex HEX)
string(LENGTH "${_record_hex}" _record_hex_length)
math(EXPR _record_size "${_record_hex_length} / 2")
set(_record_bytes "")
set(_offset 0)
while(_offset LESS _record_hex_length)
    string(SUBSTRING "${_record_hex}" ${_offset} 64 chunk)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," chunk "${chunk}")
    string(REGEX REPLACE ",$" "" chunk "${chunk}")
    string(APPEND _record_bytes "\t.byte ${chunk}\n")
    math(EXPR _offset "${_offset} + 64")
endwhile()
set(LIBCHECKPOINT_CONTRACT_RECORD_SOURCE "${LIBCHECKPOINT_CONTRACT_DIR}/runtime_contract_record.S")
file(WRITE "${LIBCHECKPOINT_CONTRACT_RECORD_SOURCE}"
"/* Generated by cmake/RuntimeContract.cmake: this archive's runtime record. */
#include \"runtime_contract.h\"
#include \"runtime_contract_fingerprint.h\"

\t.section libcheckpoint_contract,\"a\"
\t.balign 8
\t.globl libcheckpoint_runtime_contract
\t.type libcheckpoint_runtime_contract, %object
\t.globl LIBCHECKPOINT_CONTRACT_ANCHOR
\t.type LIBCHECKPOINT_CONTRACT_ANCHOR, %object
libcheckpoint_runtime_contract:
LIBCHECKPOINT_CONTRACT_ANCHOR:
\t.4byte LIBCHECKPOINT_CONTRACT_MAGIC
\t.2byte LIBCHECKPOINT_CONTRACT_VERSION
\t.2byte LIBCHECKPOINT_CONTRACT_KIND_RUNTIME
\t.4byte LIBCHECKPOINT_CONTRACT_HEADER_SIZE
\t.4byte ${_record_size}
\t.8byte LIBCHECKPOINT_CONTRACT_FINGERPRINT
\t.8byte LIBCHECKPOINT_RUNTIME_CAPABILITIES
\t/* Extracting this record from the archive extracts the check too. */
\t.8byte libcheckpoint_check_runtime_contract
${_record_bytes}\t.balign 8, 0
\t.size libcheckpoint_runtime_contract, . - libcheckpoint_runtime_contract
\t.size LIBCHECKPOINT_CONTRACT_ANCHOR, . - LIBCHECKPOINT_CONTRACT_ANCHOR
\t.section .note.GNU-stack,\"\",%progbits
")

# The manifest of one archive: write lib<archive>.contract.json.
function(libcheckpoint_write_contract_manifest archive prefix)
    set(bits "${${prefix}_capabilities}")
    set(capabilities "")
    set(shift 0)
    foreach(capability IN LISTS _contract_capability_names)
        math(EXPR set "(${bits} >> ${shift}) & 1")
        if(set EQUAL 1)
            list(APPEND capabilities "    \"${capability}\": true")
        else()
            list(APPEND capabilities "    \"${capability}\": false")
        endif()
        math(EXPR shift "${shift} + 1")
    endforeach()
    string(REPLACE ";" ",\n" capabilities "${capabilities}")
    set(runtime "")
    foreach(field IN LISTS _contract_runtime)
        string(REPLACE "|" ";" parts "${field}")
        list(GET parts 0 key)
        if(NOT key STREQUAL "capabilities")
            list(APPEND runtime "    \"${key}\": ${${prefix}_${key}}")
        endif()
    endforeach()
    string(REPLACE ";" ",\n" runtime "${runtime}")
    set(manifest "{
  \"schema\": \"libcheckpoint-runtime-contract\",
  \"version\": ${_contract_version},
  \"archive\": \"lib${archive}.a\",
  \"fingerprint\": \"${LIBCHECKPOINT_CONTRACT_FINGERPRINT}\",
  \"anchor\": \"${LIBCHECKPOINT_CONTRACT_ANCHOR}\",
  \"abi\": {
${_abi_json}
  },
  \"capability_bits\": ${bits},
  \"capabilities\": {
${capabilities}
  },
  \"runtime\": {
${runtime}
  },
  \"provenance\": {
    \"compiler\": \"${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\",
    \"build_type\": \"${CMAKE_BUILD_TYPE}\",
    \"x64_vector_state\": \"${TEAPOT_X64_VECTOR_STATE}\",
    \"dift_layout\": \"${TEAPOT_DIFT_LAYOUT}\"
  }
}
")
    set(path "${CMAKE_CURRENT_BINARY_DIR}/lib${archive}.contract.json")
    file(WRITE "${path}" "${manifest}")
    install(FILES "${path}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/runtime_contract_fingerprint.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
