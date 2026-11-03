#include "checkpoint.h"
#include <asm/unistd.h>

/* A continuation, not a C call. Save syscall clobbers on the application stack
 * (below the x64 red zone), and copy all shared state before unblocking. Once
 * the syscall returns, a handler may freely checkpoint using runtime scratch.
 * x64 jumps through teapot_signal_resume_target itself: once its frame is
 * released, a nested signal frame on the same stack may cover it, and
 * forwarded handlers give the target back unchanged.
 * No vector instructions are used: the kernel preserves FP/vector state. */
    .text
    .globl teapot_signal_resume
    .globl teapot_signal_resume_mask_load
    .globl teapot_signal_resume_mask_ready
    .globl teapot_signal_resume_unblocked
#if defined(__x86_64__)
    .type teapot_signal_resume,@function
teapot_signal_resume:
    leaq -208(%rsp), %rsp
    movq %rax, 16(%rsp)
    movq %rdi, 24(%rsp)
    movq %rsi, 32(%rsp)
    movq %rdx, 40(%rsp)
    movq %r10, 48(%rsp)
    movq %rcx, 56(%rsp)
    movq %r11, 64(%rsp)
    pushfq
    popq %rax
    movq %rax, 72(%rsp)
teapot_signal_resume_mask_load:
    movq teapot_pending_signals(%rip), %rax
    movq %rax, (%rsp)
teapot_signal_resume_mask_ready:
    movq $0, teapot_pending_signals(%rip)
    movl $__NR_rt_sigprocmask, %eax
    movl $1, %edi /* SIG_UNBLOCK */
    movq %rsp, %rsi
    xorl %edx, %edx
    movl $8, %r10d
    syscall
teapot_signal_resume_unblocked:
    movq 72(%rsp), %rax
    pushq %rax
    popfq
    movq 16(%rsp), %rax
    movq 24(%rsp), %rdi
    movq 32(%rsp), %rsi
    movq 40(%rsp), %rdx
    movq 48(%rsp), %r10
    movq 56(%rsp), %rcx
    movq 64(%rsp), %r11
    leaq 208(%rsp), %rsp
    jmp *teapot_signal_resume_target(%rip)
#elif defined(__aarch64__)
    .type teapot_signal_resume,%function
teapot_signal_resume:
    sub sp, sp, #80
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x8, x17, [sp, #48]
    mrs x17, nzcv
    str x17, [sp, #64]
    ldr x17, =teapot_signal_resume_target
    ldr x17, [x17]
    str x17, [sp, #8]
teapot_signal_resume_mask_load:
    ldr x17, =teapot_pending_signals
    ldr x0, [x17]
    str x0, [sp]
teapot_signal_resume_mask_ready:
    str xzr, [x17]
    mov x0, #1 /* SIG_UNBLOCK */
    mov x1, sp
    mov x2, #0
    mov x3, #8
    mov x8, #__NR_rt_sigprocmask
    svc #0
teapot_signal_resume_unblocked:
    ldr x17, [sp, #64]
    msr nzcv, x17
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x8, x17, [sp, #48]
    ldr x16, [sp, #8]
    add sp, sp, #80
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    ret x16
#else
    br x16
#endif
#elif defined(__riscv) && __riscv_xlen == 64
    .option push
    .option norelax
    .type teapot_signal_resume,@function
teapot_signal_resume:
    addi sp, sp, -64
    sd a0, 16(sp)
    sd a1, 24(sp)
    sd a2, 32(sp)
    sd a3, 40(sp)
    sd a7, 48(sp)
    lla t0, teapot_signal_resume_target
    ld t0, 0(t0)
    sd t0, 8(sp)
teapot_signal_resume_mask_load:
    lla t0, teapot_pending_signals
    ld a0, 0(t0)
    sd a0, 0(sp)
teapot_signal_resume_mask_ready:
    sd zero, 0(t0)
    li a0, 1 /* SIG_UNBLOCK */
    mv a1, sp
    li a2, 0
    li a3, 8
    li a7, __NR_rt_sigprocmask
    ecall
teapot_signal_resume_unblocked:
    ld a0, 16(sp)
    ld a1, 24(sp)
    ld a2, 32(sp)
    ld a3, 40(sp)
    ld a7, 48(sp)
    ld t0, 8(sp)
    addi sp, sp, 64
    jr t0
    .option pop
#endif
    .size teapot_signal_resume, .-teapot_signal_resume
    .section .note.GNU-stack,""
