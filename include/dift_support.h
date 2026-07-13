#pragma once

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#endif

#include "config.h"

#define ENABLE_DIFT

#define TAG_ATTACKER 1
#define TAG_ATTACKER_INDIRECT 2
#define TAG_SECRET 16
#define TAG_SECRET_INDIRECT 32

#ifndef __ASSEMBLER__
typedef uint8_t dift_tag_t;
#endif

#define DIFT_REG_TAGS_SIZE 48

#ifndef __ASSEMBLER__
// 0~15 = rax~r15, 16~47=zmm0~zmm31
extern dift_tag_t dift_reg_tags[DIFT_REG_TAGS_SIZE];
extern dift_tag_t dift_reg_queued_tags[DIFT_REG_TAGS_SIZE];
#endif

#if defined(__x86_64__)
# define DIFT_REG_RAX 0
# define DIFT_REG_RBX 1
# define DIFT_REG_RCX 2
# define DIFT_REG_RDX 3
# define DIFT_REG_RSI 4
# define DIFT_REG_RDI 5
# define DIFT_REG_RSP 6
# define DIFT_REG_RBP 7
# define DIFT_REG_R8 8
# define DIFT_REG_R9 9
# define DIFT_REG_R10 10
# define DIFT_REG_R11 11
# define DIFT_REG_R12 12
# define DIFT_REG_R13 13
# define DIFT_REG_R14 14
# define DIFT_REG_R15 15
// TODO: xmm registers

# define DIFT_ARG0 DIFT_REG_RDI
# define DIFT_ARG1 DIFT_REG_RSI
# define DIFT_ARG2 DIFT_REG_RDX
# define DIFT_ARG3 DIFT_REG_RCX
# define DIFT_ARG4 DIFT_REG_R8
# define DIFT_ARG5 DIFT_REG_R9
# define DIFT_RET DIFT_REG_RAX
#elif defined(__aarch64__)
# define DIFT_REG_X0 0
# define DIFT_REG_X1 1
# define DIFT_REG_X2 2
# define DIFT_REG_X3 3
# define DIFT_REG_X4 4
# define DIFT_REG_X5 5
# define DIFT_REG_X6 6
# define DIFT_REG_X7 7
# define DIFT_REG_X8 8
# define DIFT_REG_X9 9
# define DIFT_REG_X10 10
# define DIFT_REG_X11 11
# define DIFT_REG_X12 12
# define DIFT_REG_X13 13
# define DIFT_REG_X14 14
# define DIFT_REG_X15 15
# define DIFT_REG_X16 16
# define DIFT_REG_X17 17
# define DIFT_REG_X18 18
# define DIFT_REG_X19 19
# define DIFT_REG_X20 20
# define DIFT_REG_X21 21
# define DIFT_REG_X22 22
# define DIFT_REG_X23 23
# define DIFT_REG_X24 24
# define DIFT_REG_X25 25
# define DIFT_REG_X26 26
# define DIFT_REG_X27 27
# define DIFT_REG_X28 28
# define DIFT_REG_X29 29
# define DIFT_REG_X30 30
# define DIFT_REG_SP 31

# define DIFT_ARG0 DIFT_REG_X0
# define DIFT_ARG1 DIFT_REG_X1
# define DIFT_ARG2 DIFT_REG_X2
# define DIFT_ARG3 DIFT_REG_X3
# define DIFT_ARG4 DIFT_REG_X4
# define DIFT_ARG5 DIFT_REG_X5
# define DIFT_RET DIFT_REG_X0
#elif defined(__riscv) && __riscv_xlen == 64
# define DIFT_REG_ZERO 0
# define DIFT_REG_RA 1
# define DIFT_REG_SP 2
# define DIFT_REG_GP 3
# define DIFT_REG_TP 4
# define DIFT_REG_T0 5
# define DIFT_REG_T1 6
# define DIFT_REG_T2 7
# define DIFT_REG_S0 8
# define DIFT_REG_S1 9
# define DIFT_REG_A0 10
# define DIFT_REG_A1 11
# define DIFT_REG_A2 12
# define DIFT_REG_A3 13
# define DIFT_REG_A4 14
# define DIFT_REG_A5 15
# define DIFT_REG_A6 16
# define DIFT_REG_A7 17
# define DIFT_REG_S2 18
# define DIFT_REG_S3 19
# define DIFT_REG_S4 20
# define DIFT_REG_S5 21
# define DIFT_REG_S6 22
# define DIFT_REG_S7 23
# define DIFT_REG_S8 24
# define DIFT_REG_S9 25
# define DIFT_REG_S10 26
# define DIFT_REG_S11 27
# define DIFT_REG_T3 28
# define DIFT_REG_T4 29
# define DIFT_REG_T5 30
# define DIFT_REG_T6 31

# define DIFT_ARG0 DIFT_REG_A0
# define DIFT_ARG1 DIFT_REG_A1
# define DIFT_ARG2 DIFT_REG_A2
# define DIFT_ARG3 DIFT_REG_A3
# define DIFT_ARG4 DIFT_REG_A4
# define DIFT_ARG5 DIFT_REG_A5
# define DIFT_RET DIFT_REG_A0
#else
# error "Unsupported DIFT architecture"
#endif

#ifndef __ASSEMBLER__
#ifndef DIFT_XOR_MASK
# error "DIFT_XOR_MASK must be defined by the selected Teapot DIFT layout"
#endif

#if defined(__aarch64__)
# define DIFT_APP_ADDR(addr) ((uintptr_t)(addr) & 0x00ffffffffffffffULL)
#else
# define DIFT_APP_ADDR(addr) ((uintptr_t)(addr))
#endif

#define DIFT_MEM_ADDR(addr) ((dift_tag_t*)(DIFT_APP_ADDR(addr) ^ (uintptr_t)DIFT_XOR_MASK))
#define DIFT_MEM_TAG(addr) (*(DIFT_MEM_ADDR(addr)))

void map_dift_pages();
void map_runtime_shadow_range(uintptr_t start, uintptr_t end, int prot);
void dift_set_mem_tags(void *addr, dift_tag_t tag, size_t len);
void dift_copy_mem_tags(void *dest, const void *src, size_t len);
void dift_move_mem_tags(void *dest, const void *src, size_t len);
void dift_taint_args(int argc, char **argv);
#endif
