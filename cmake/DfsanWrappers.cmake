# LLVM DFSan's libc wrappers and the bridges to them
# (src/dift_wrappers/dfsan_wrappers.cpp), built into each runtime archive with
# that archive's settings. The C compiler builds them as C++, so every existing
# cross configuration works unchanged. objcopy then keeps only the
# *__dift_wrapper__ bridges global: the upstream code's sanitizer and DFSan
# symbols must not bind to a sanitizer runtime linked into the same program.
find_program(TEAPOT_LLVM_OBJCOPY NAMES llvm-objcopy)
if(TEAPOT_LLVM_OBJCOPY)
    set(TEAPOT_DFSAN_OBJCOPY "${TEAPOT_LLVM_OBJCOPY}")
elseif(CMAKE_OBJCOPY)
    set(TEAPOT_DFSAN_OBJCOPY "${CMAKE_OBJCOPY}")
else()
    message(FATAL_ERROR "llvm-objcopy or the toolchain's objcopy is required for the DFSan wrappers")
endif()
set_source_files_properties(src/dift_wrappers/dfsan_wrappers.cpp PROPERTIES LANGUAGE C)

# Object library NAME and the localized object NAME.o beside it; the caller
# sets the shadow layout and runtime definitions.
function(teapot_dfsan_wrapper_object name)
    add_library(${name} OBJECT src/dift_wrappers/dfsan_wrappers.cpp)
    set_target_properties(${name} PROPERTIES TEAPOT_COMPILE_AS_CXX ON)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dift_wrappers/dfsan_platform"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm-dfsan/compiler-rt/lib")
    target_compile_options(${name} PRIVATE -x c++ -std=gnu++17 -fno-exceptions -fno-rtti
        -funsigned-char -Wno-attribute-alias)
    # Upstream's no-op SanitizerCoverage callbacks, renamed: the runtime's
    # coverage contract allows only undefined references to these names.
    target_compile_definitions(${name} PRIVATE
        __sanitizer_cov_trace_pc_guard=teapot_dfsan_unused_cov_trace_pc_guard
        __sanitizer_cov_trace_pc_guard_init=teapot_dfsan_unused_cov_trace_pc_guard_init
        __sanitizer_cov_trace_pc_indir=teapot_dfsan_unused_cov_trace_pc_indir
        __sanitizer_cov_pcs_init=teapot_dfsan_unused_cov_pcs_init)
    set(object "${CMAKE_CURRENT_BINARY_DIR}/${name}.o")
    add_custom_command(OUTPUT "${object}"
        COMMAND "${TEAPOT_DFSAN_OBJCOPY}" --wildcard "--keep-global-symbol=*__dift_wrapper__"
                "$<TARGET_OBJECTS:${name}>" "${object}"
        DEPENDS ${name} "$<TARGET_OBJECTS:${name}>"
        VERBATIM)
    set_source_files_properties("${object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endfunction()

foreach(runtime IN ITEMS checkpoint checkpoint_nested)
    if(TARGET ${runtime})
        teapot_dfsan_wrapper_object(${runtime}_dfsan_wrappers)
        teapot_configure_checkpoint_target(${runtime}_dfsan_wrappers)
        target_sources(${runtime} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${runtime}_dfsan_wrappers.o")
    endif()
endforeach()
