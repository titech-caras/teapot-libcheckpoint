set(TEAPOT_DIFT_RUNTIME_DEFS)
if(NOT TEAPOT_ENABLE_DIFT_RUNTIME)
    list(APPEND TEAPOT_DIFT_RUNTIME_DEFS DISABLE_DIFT_RUNTIME)
endif()

set(TEAPOT_ARCH_RUNTIME_DEFS)
if(TEAPOT_ENABLE_RISCV_FLOAT_STATE)
    list(APPEND TEAPOT_ARCH_RUNTIME_DEFS ENABLE_RISCV_FLOAT_STATE)
endif()

set(CHECKPOINT_RUNTIME_SOURCES
    src/checkpoint.c
    ${CHECKPOINT_ASM_SOURCE}
    src/signal_handler.c
    src/dift_support.c
    src/dift_wrappers/dift_wrappers.c
    src/report_gadget.c)
set_source_files_properties(${CHECKPOINT_ASM_SOURCE} PROPERTIES COMPILE_FLAGS -O0)

function(teapot_configure_checkpoint_target target)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_compile_options(${target} PRIVATE -fno-stack-protector)
    target_compile_definitions(${target} PRIVATE
        DIFT_XOR_MASK=${DIFT_XOR_MASK}
        CHECKPOINT_ASAN_SHADOW_OFFSET=${DIFT_ASAN_SHADOW_OFFSET}
        CHECKPOINT_ASAN_LOW_APP_LIMIT=${DIFT_ASAN_SHADOW_OFFSET}
        AARCH64_SHADOW_STACK_SIZE=${TEAPOT_AARCH64_SHADOW_STACK_SIZE}ULL
        AARCH64_SHADOW_STACK_CONTROL_OFFSET=${TEAPOT_AARCH64_SHADOW_STACK_CONTROL_OFFSET}
        ${TEAPOT_DIFT_RUNTIME_DEFS}
        ${TEAPOT_ARCH_RUNTIME_DEFS}
        ${DIFT_RANGE_DEFS})
endfunction()

add_library(checkpoint ${CHECKPOINT_RUNTIME_SOURCES})
teapot_configure_checkpoint_target(checkpoint)
install(TARGETS checkpoint ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(TEAPOT_BUILD_NESTED_RUNTIME)
    add_library(checkpoint_nested ${CHECKPOINT_RUNTIME_SOURCES})
    teapot_configure_checkpoint_target(checkpoint_nested)
    target_compile_definitions(checkpoint_nested PRIVATE ENABLE_NESTED_SPECULATION)
    install(TARGETS checkpoint_nested ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
