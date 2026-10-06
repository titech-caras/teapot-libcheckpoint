set(TEAPOT_DIFT_RUNTIME_DEFS)
if(NOT TEAPOT_ENABLE_DIFT_RUNTIME)
    list(APPEND TEAPOT_DIFT_RUNTIME_DEFS DISABLE_DIFT_RUNTIME)
endif()

set(TEAPOT_ARCH_RUNTIME_DEFS)
list(APPEND TEAPOT_ARCH_RUNTIME_DEFS TEAPOT_X64_VECTOR_MODE=${_vector_mode})
if(TEAPOT_ENABLE_COVERAGE)
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS COVERAGE)
endif()
if(TEAPOT_ENABLE_FAULT_TRAINING)
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS ENABLE_FAULT_TRAINING)
endif()
if(TEAPOT_ENABLE_FAULT_PUBLISHING)
    if(NOT CHECKPOINT_ARCH_NAME MATCHES "^(x64|aarch64|riscv64)$" OR NOT TEAPOT_ENABLE_FAULT_TRAINING)
        message(FATAL_ERROR "Fault publishing requires a supported 64-bit ISA and TEAPOT_ENABLE_FAULT_TRAINING=ON")
    endif()
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS ENABLE_FAULT_PUBLISHING)
endif()
if(TEAPOT_EXPERIMENTAL_AARCH64_BTI)
    if(NOT CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
        message(FATAL_ERROR "The BTI experiment requires AArch64")
    endif()
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS TEAPOT_EXPERIMENTAL_AARCH64_BTI)
endif()
if(TEAPOT_ENABLE_RISCV_FLOAT_STATE)
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS ENABLE_RISCV_FLOAT_STATE)
endif()
if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "mte")
    if(NOT CHECKPOINT_ARCH_NAME STREQUAL "aarch64")
        message(FATAL_ERROR "TEAPOT_AARCH64_TAG_STORAGE=mte is only valid for AArch64")
    endif()
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS TEAPOT_AARCH64_MTE_TAG_STORAGE)
elseif(NOT TEAPOT_AARCH64_TAG_STORAGE STREQUAL "shadow")
    message(FATAL_ERROR "Unsupported TEAPOT_AARCH64_TAG_STORAGE=${TEAPOT_AARCH64_TAG_STORAGE}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/RuntimeContract.cmake")

set(CHECKPOINT_RUNTIME_SOURCES
    "${LIBCHECKPOINT_CONTRACT_RECORD_SOURCE}"
    src/checkpoint.c
    asm/storage.S
    ${CHECKPOINT_ASM_SOURCE}
    src/signal_handler.c
    src/fault_sites.c
    src/fault_x64.c
    src/dift_support.c
    src/dift_wrappers/dift_wrappers.c
    src/report_gadget.c)
if(TEAPOT_EXPERIMENTAL_AARCH64_BTI)
    list(APPEND CHECKPOINT_RUNTIME_SOURCES src/aarch64_bti.c asm/aarch64_bti.S src/aarch64_pac.c)
endif()

function(teapot_configure_checkpoint_target target)
    if(TEAPOT_ENABLE_FAULT_TRAINING)
        # AArch64/RV GNU ld may otherwise put readonly metadata in the RX
        # segment. The runtime deliberately requires R-only table mappings.
        target_link_options(${target} PUBLIC -Wl,-z,separate-code)
    endif()
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    # Keep runtime safety preconditions enabled without replacing the user's
    # standard Debug/Release/RelWithDebInfo/MinSizeRel flags.
    target_compile_options(${target} PRIVATE -fno-stack-protector -UNDEBUG)
    target_compile_definitions(${target} PRIVATE
        DIFT_XOR_MASK=${DIFT_XOR_MASK}
        ${TEAPOT_DIFT_RUNTIME_DEFS}
        ${TEAPOT_ARCH_RUNTIME_DEFS}
        ${DIFT_RANGE_DEFS})
    if(TEAPOT_AARCH64_TAG_STORAGE STREQUAL "mte")
        target_compile_options(${target} PRIVATE -march=armv8.5-a+memtag)
    endif()
endfunction()

add_library(checkpoint ${CHECKPOINT_RUNTIME_SOURCES})
teapot_configure_checkpoint_target(checkpoint)
install(TARGETS checkpoint ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
libcheckpoint_write_contract_manifest(checkpoint _contract_default)

if(TEAPOT_BUILD_NESTED_RUNTIME)
    add_library(checkpoint_nested ${CHECKPOINT_RUNTIME_SOURCES})
    teapot_configure_checkpoint_target(checkpoint_nested)
    target_compile_definitions(checkpoint_nested PRIVATE ENABLE_NESTED_SPECULATION)
    install(TARGETS checkpoint_nested ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    libcheckpoint_write_contract_manifest(checkpoint_nested _contract_nested)
endif()
