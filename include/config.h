#pragma once

#ifdef ENABLE_NESTED_SPECULATION
#define MAX_CHECKPOINTS 6
#define SPECFUZZ_PRIORITIZED_SIMULATION
#define BRANCH_FULL_EXEC_COUNT 5
#else
#define MAX_CHECKPOINTS 1
#endif

#define SILENCE_GADGET_AFTER_FIRST_DISCOVERY

#if defined(__aarch64__)
#define ENABLE_AARCH64_SIMD_STATE
#endif

#ifndef __ASSEMBLER__
#define LIBCHECKPOINT_PROTECTED_SECTION \
    __attribute__((section("teapot_protected"), used))
#define LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(alignment) \
    __attribute__((section("teapot_protected"), used, aligned(alignment)))
#endif

// Define only for riscv64 targets built with floating-point register support.
// The base riscv64 ISA does not guarantee floating-point registers.
//#define ENABLE_RISCV_FLOAT_STATE
