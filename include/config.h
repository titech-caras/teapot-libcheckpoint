#pragma once

/* Runtime implementation fact, not a checkpoint-layout ABI change. */
#define LIBCHECKPOINT_SIGNAL_INSTALLER_COVERAGE 1

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

#ifdef TEAPOT_RISCV_LP64_PURE
#if !defined(__riscv) || __riscv_xlen != 64 || !defined(__riscv_float_abi_soft)
#error "lp64-pure requires the RV64 LP64 calling ABI"
#endif
#if defined(ENABLE_RISCV_FLOAT_STATE) || defined(TEAPOT_EXPERIMENTAL_AARCH64_BTI)
#error "lp64-pure forbids FP checkpoint state and AArch64 BTI/PAC"
#endif
#if defined(__riscv_flen) || defined(__riscv_vector) || defined(__riscv_zfh) || \
    defined(__riscv_zfhmin) || defined(__riscv_zfa) || defined(__riscv_zfinx) || \
    defined(__riscv_zdinx) || defined(__riscv_zhinx) || defined(__riscv_zhinxmin) || \
    defined(__riscv_zba) || defined(__riscv_zbb) || defined(__riscv_zbc) || defined(__riscv_zbs) || \
    defined(__riscv_zbkb) || defined(__riscv_zbkc) || defined(__riscv_zbkx)
#error "lp64-pure requires the approved IMAC/Zicsr/Zifencei/Zicond ISA without FP, vector or B"
#endif
#if !defined(__riscv_mul) || !defined(__riscv_atomic) || !defined(__riscv_compressed) || \
    !defined(__riscv_zicsr) || !defined(__riscv_zifencei) || !defined(__riscv_zicond)
#error "lp64-pure requires the published default IMAC/Zicsr/Zifencei/Zicond baseline"
#endif
#endif
