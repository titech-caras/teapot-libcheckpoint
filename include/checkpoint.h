#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

#include "config.h"
#include "dift_support.h"

//==========config===========
//#define VERBOSE
//#define VERBOSE_DBGINFO
#define COVERAGE
//#define TIME
//===========================


#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define MEM_HISTORY_LEN 1048576
#define MEM_HISTORY_ENTRY_SIZE 24
#define MEM_HISTORY_ADDR_OFFSET 0
#define MEM_HISTORY_DATA_OFFSET 8
#define MEM_HISTORY_SIZE_OFFSET 16
#define MEM_HISTORY_MTE_TAG_SIZE 0xff
#define GUARD_LIST_LEN 1048576
#define SCRATCHPAD_SIZE 1048576
#define SCRATCHPAD_FIRST_SPILL_OFFSET (SCRATCHPAD_SIZE / 2)
#define RISCV64_ORIGINAL_TP_OFFSET (SCRATCHPAD_SIZE - 32768)

#if defined(SPECFUZZ_PRIORITIZED_SIMULATION) || defined(BRANCH_FULL_EXEC_COUNT)
#define USE_BRANCH_EXEC_COUNT
#endif

#define SCRATCHPAD_TOP "scratchpad+" STR(SCRATCHPAD_SIZE - 8)

#define SWITCH_TO_SCRATCHPAD_STACK "mov %rsp, old_rsp\n" "lea " SCRATCHPAD_TOP ", %rsp\n"
#define SWITCH_TO_ORIGINAL_STACK "mov old_rsp, %rsp\n"

#define CHECKPOINT_TARGET_TRAMPOLINE_ADDR 0
#define CHECKPOINT_TARGET_RETURN_ADDR 8
#define CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR 16
#define CHECKPOINT_TARGET_SCRATCH_REG_ADDR 24
#define CHECKPOINT_TARGET_METADATA_SIZE 32

#define PROCESSOR_EXTENDED_STATE_SIZE 2048
#define PROCESSOR_EXTENDED_STATE_SHIFT 11

#if defined(__x86_64__)
#define CHECKPOINT_METADATA_SIZE 256
#define CHECKPOINT_METADATA_SHIFT 8
#define CKPT_RAX 0
#define CKPT_RBX 8
#define CKPT_RCX 16
#define CKPT_RDX 24
#define CKPT_RSI 32
#define CKPT_RDI 40
#define CKPT_RSP 48
#define CKPT_RBP 56
#define CKPT_R8 64
#define CKPT_R9 72
#define CKPT_R10 80
#define CKPT_R11 88
#define CKPT_R12 96
#define CKPT_R13 104
#define CKPT_R14 112
#define CKPT_R15 120
#define CKPT_FLAGS 128
#define CKPT_INSTRUCTION_CNT 136
#define CKPT_MEMORY_HISTORY_TOP 144
#define CKPT_RETURN_ADDRESS 152
#define CKPT_DIFT_REG_TAGS 160
#define CKPT_GUARD_LIST_TOP 208
#elif defined(__aarch64__)
#define CHECKPOINT_METADATA_SIZE 512
#define CHECKPOINT_METADATA_SHIFT 9
#define CKPT_AARCH64_X0 0
#define CKPT_AARCH64_X1 8
#define CKPT_AARCH64_X2 16
#define CKPT_AARCH64_X3 24
#define CKPT_AARCH64_X4 32
#define CKPT_AARCH64_X5 40
#define CKPT_AARCH64_X6 48
#define CKPT_AARCH64_X7 56
#define CKPT_AARCH64_X8 64
#define CKPT_AARCH64_X9 72
#define CKPT_AARCH64_X10 80
#define CKPT_AARCH64_X11 88
#define CKPT_AARCH64_X12 96
#define CKPT_AARCH64_X13 104
#define CKPT_AARCH64_X14 112
#define CKPT_AARCH64_X15 120
#define CKPT_AARCH64_X16 128
#define CKPT_AARCH64_X17 136
#define CKPT_AARCH64_X18 144
#define CKPT_AARCH64_X19 152
#define CKPT_AARCH64_X20 160
#define CKPT_AARCH64_X21 168
#define CKPT_AARCH64_X22 176
#define CKPT_AARCH64_X23 184
#define CKPT_AARCH64_X24 192
#define CKPT_AARCH64_X25 200
#define CKPT_AARCH64_X26 208
#define CKPT_AARCH64_X27 216
#define CKPT_AARCH64_X28 224
#define CKPT_AARCH64_X29 232
#define CKPT_AARCH64_X30 240
#define CKPT_AARCH64_SP 248
#define CKPT_AARCH64_NZCV 256
#define CKPT_INSTRUCTION_CNT 264
#define CKPT_MEMORY_HISTORY_TOP 272
#define CKPT_RETURN_ADDRESS 280
#define CKPT_DIFT_REG_TAGS 288
#define CKPT_GUARD_LIST_TOP 336
#elif defined(__riscv) && __riscv_xlen == 64
#define CHECKPOINT_METADATA_SIZE 512
#define CHECKPOINT_METADATA_SHIFT 9
#define CKPT_RISCV_X1 0
#define CKPT_RISCV_X2 8
#define CKPT_RISCV_X3 16
#define CKPT_RISCV_X4 24
#define CKPT_RISCV_X5 32
#define CKPT_RISCV_X6 40
#define CKPT_RISCV_X7 48
#define CKPT_RISCV_X8 56
#define CKPT_RISCV_X9 64
#define CKPT_RISCV_X10 72
#define CKPT_RISCV_X11 80
#define CKPT_RISCV_X12 88
#define CKPT_RISCV_X13 96
#define CKPT_RISCV_X14 104
#define CKPT_RISCV_X15 112
#define CKPT_RISCV_X16 120
#define CKPT_RISCV_X17 128
#define CKPT_RISCV_X18 136
#define CKPT_RISCV_X19 144
#define CKPT_RISCV_X20 152
#define CKPT_RISCV_X21 160
#define CKPT_RISCV_X22 168
#define CKPT_RISCV_X23 176
#define CKPT_RISCV_X24 184
#define CKPT_RISCV_X25 192
#define CKPT_RISCV_X26 200
#define CKPT_RISCV_X27 208
#define CKPT_RISCV_X28 216
#define CKPT_RISCV_X29 224
#define CKPT_RISCV_X30 232
#define CKPT_RISCV_X31 240
#define CKPT_INSTRUCTION_CNT 248
#define CKPT_MEMORY_HISTORY_TOP 256
#define CKPT_RETURN_ADDRESS 264
#define CKPT_DIFT_REG_TAGS 272
#define CKPT_GUARD_LIST_TOP 320
#else
#error "Unsupported libcheckpoint architecture"
#endif

#if defined(__clang__) && (defined(__x86_64__) || defined(__aarch64__))
#define LIBCHECKPOINT_PRESERVE_MOST __attribute__((preserve_most))
#else
#define LIBCHECKPOINT_PRESERVE_MOST
#endif

#define ROLLBACK_ROB_LEN 0
//#define ROLLBACK_ASAN 1
#define ROLLBACK_SIGSEGV 2
#define ROLLBACK_EXT_LIB 3
#define ROLLBACK_MALFORMED_INDIRECT_BR 4

#ifndef __ASSEMBLER__

typedef struct memory_history {
    void *addr;
    uint64_t data;
    uint8_t size;
    uint8_t padding[7];
} memory_history_t;

typedef struct checkpoint_register_state {
#if defined(__x86_64__)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t flags;
#elif defined(__aarch64__)
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp, nzcv;
#elif defined(__riscv) && __riscv_xlen == 64
    uint64_t x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29, x30, x31;
#endif
} checkpoint_register_state_t;

typedef checkpoint_register_state_t general_register_state_t;

typedef __attribute__((aligned(CHECKPOINT_METADATA_SIZE))) struct checkpoint_metadata {
    checkpoint_register_state_t registers;
    uint64_t instruction_cnt;
    memory_history_t *memory_history_top;
    uint64_t return_address;

    dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];

    uint32_t *guard_list_top;

    uint64_t alignment[(CHECKPOINT_METADATA_SIZE - CKPT_GUARD_LIST_TOP - 8) / 8];
} checkpoint_metadata_t;

typedef struct statistics {
    struct {
        uint64_t normal_time, spec_time, ckpt_time, rstr_time;
    } rdtsc_runtime;

    uint64_t total_ckpt;
    uint64_t ckpt_depth[MAX_CHECKPOINTS];
    uint64_t rollback_reason[5];

    uint64_t total_bug;
    uint64_t bug_type[100];
} statistics_t;

typedef __attribute__((aligned(64))) struct xsave_area {
    char data[PROCESSOR_EXTENDED_STATE_SIZE];
} xsave_area_t;

typedef __attribute__((aligned(16))) uint8_t scratchpad_t[SCRATCHPAD_SIZE];

/*#define GADGET_SPECFUZZ_ASAN_READ 1
#define GADGET_SPECFUZZ_ASAN_WRITE 3
#define GADGET_SIGSEGV 11
#define GADGET_SPECTAINT_BCB 21
#define GADGET_SPECTAINT_BCBS 22
*/
#define GADGET_KASPER_MDS 41
#define GADGET_KASPER_CACHE 42
#define GADGET_KASPER_PORT 43

extern scratchpad_t scratchpad;
extern checkpoint_metadata_t checkpoint_metadata[MAX_CHECKPOINTS];
extern uint32_t guard_list[GUARD_LIST_LEN];
extern uint64_t checkpoint_cnt, instruction_cnt;
extern uint64_t indirect_branch_flags_scratch;
extern statistics_t simulation_statistics;
extern uint64_t last_rdtsc;
extern bool libcheckpoint_enabled;

extern volatile bool in_restore_memlog;

LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_enable(int argc, char **argv);
LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_disable();

#if defined(__x86_64__)
__attribute__((noreturn)) void make_checkpoint_x64();
#elif defined(__aarch64__)
__attribute__((noreturn)) void make_checkpoint_aarch64();
#elif defined(__riscv) && __riscv_xlen == 64
__attribute__((noreturn)) void make_checkpoint_riscv64();
#endif
void add_instruction_counter_check_restore();
__attribute__((noreturn)) void restore_checkpoint(int type);
void restore_checkpoint_memlog();
__attribute__((noreturn)) void restore_checkpoint_after_memlog();
__attribute__((noreturn)) void restore_checkpoint_registers();

__attribute__((noreturn)) void restore_checkpoint_SIGSEGV();

_Static_assert(offsetof(checkpoint_metadata_t, instruction_cnt) == CKPT_INSTRUCTION_CNT,
        "checkpoint_metadata_t instruction counter offset mismatch");
_Static_assert(offsetof(checkpoint_metadata_t, memory_history_top) == CKPT_MEMORY_HISTORY_TOP,
        "checkpoint_metadata_t memory history offset mismatch");
_Static_assert(offsetof(checkpoint_metadata_t, return_address) == CKPT_RETURN_ADDRESS,
        "checkpoint_metadata_t return address offset mismatch");
_Static_assert(offsetof(checkpoint_metadata_t, dift_reg_tags) == CKPT_DIFT_REG_TAGS,
        "checkpoint_metadata_t DIFT offset mismatch");
_Static_assert(offsetof(checkpoint_metadata_t, guard_list_top) == CKPT_GUARD_LIST_TOP,
        "checkpoint_metadata_t guard list offset mismatch");
_Static_assert(sizeof(checkpoint_metadata_t) == CHECKPOINT_METADATA_SIZE,
        "checkpoint_metadata_t size mismatch");
_Static_assert(offsetof(memory_history_t, addr) == MEM_HISTORY_ADDR_OFFSET,
        "memory_history_t address offset mismatch");
_Static_assert(offsetof(memory_history_t, data) == MEM_HISTORY_DATA_OFFSET,
        "memory_history_t data offset mismatch");
_Static_assert(offsetof(memory_history_t, size) == MEM_HISTORY_SIZE_OFFSET,
        "memory_history_t size offset mismatch");
_Static_assert(sizeof(memory_history_t) == MEM_HISTORY_ENTRY_SIZE,
        "memory_history_t size mismatch");
_Static_assert(sizeof(xsave_area_t) == PROCESSOR_EXTENDED_STATE_SIZE,
        "processor extended state size mismatch");

#endif
