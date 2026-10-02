#pragma once

#ifdef ENABLE_NESTED_SPECULATION
#define MAX_CHECKPOINTS 6
#define SPECFUZZ_PRIORITIZED_SIMULATION
#define BRANCH_FULL_EXEC_COUNT 5
#else
#define MAX_CHECKPOINTS 1
#endif

#define SILENCE_GADGET_AFTER_FIRST_DISCOVERY

/* The speculation budget (ROB_LEN) is Teapot's alone: its restore points
 * compare the instruction count against teapot/configs/runtime.py. The
 * runtime never reads it, so -DROB_LEN here changes nothing. */

#if defined(__aarch64__)
#define ENABLE_AARCH64_SIMD_STATE
#endif

#ifndef __ASSEMBLER__
/* Protected objects must have size/alignment divisible by eight so poisoning
 * never has to extend past the section into unrelated application storage. */
#define LIBCHECKPOINT_PROTECTED_SECTION \
    __attribute__((section("teapot_protected"), used))
#define LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(alignment) \
    __attribute__((section("teapot_protected"), used, aligned(alignment)))
/* Pair each protected declaration with a check, including arrays and flags. */
#define LIBCHECKPOINT_ASSERT_PROTECTED(object) \
    _Static_assert(sizeof(object) % 8 == 0 && __alignof__(object) % 8 == 0 && \
                   __alignof__(object) >= __alignof__(__typeof__(object)), \
                   #object " must occupy whole aligned ASan granules")
#endif

// Define only for riscv64 targets built with floating-point register support.
// The base riscv64 ISA does not guarantee floating-point registers.
//#define ENABLE_RISCV_FLOAT_STATE
