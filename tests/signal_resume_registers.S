#include "checkpoint.h"
    .text
    .globl test_signal_registers
    .globl test_signal_registers_return
#if defined(__x86_64__)
test_signal_registers:
    mov $1, %rax
    mov $2, %rdi
    mov $3, %rsi
    mov $4, %rdx
    mov $5, %r10
    mov $6, %rcx
    mov $7, %r11
    stc
    std
    pushfq
    popq test_signal_flags(%rip)
    movq $0x12345, -8(%rsp)
    movq $0x6789a, -120(%rsp)
    jmp teapot_signal_resume
test_signal_registers_return:
    mov %rax, test_signal_register_values(%rip)
    mov %rdi, test_signal_register_values+8(%rip)
    mov %rsi, test_signal_register_values+16(%rip)
    mov %rdx, test_signal_register_values+24(%rip)
    mov %r10, test_signal_register_values+32(%rip)
    mov %rcx, test_signal_register_values+40(%rip)
    mov %r11, test_signal_register_values+48(%rip)
    mov -8(%rsp), %rax
    mov %rax, test_signal_register_values+56(%rip)
    mov -120(%rsp), %rax
    mov %rax, test_signal_register_values+64(%rip)
    pushfq
    popq test_signal_register_values+72(%rip)
    cld
    ret
#elif defined(__aarch64__)
test_signal_registers:
    mov x0, #0xa0000000
    msr nzcv, x0
    ldr x1, =test_signal_flags
    str x0, [x1]
    mov x0, #1
    mov x1, #2
    mov x2, #3
    mov x3, #4
    mov x8, #5
    mov x17, #6
    b teapot_signal_resume
test_signal_registers_return:
    ldr x16, =test_signal_register_values
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x8, x17, [x16, #32]
    mrs x0, nzcv
    str x0, [x16, #48]
    ret
#else
    .option push
    .option norelax
test_signal_registers:
    li a0, 1
    li a1, 2
    li a2, 3
    li a3, 4
    li a7, 5
    li t1, 6
    li t2, 7
    j teapot_signal_resume
test_signal_registers_return:
    lla t0, test_signal_register_values
    sd a0, 0(t0)
    sd a1, 8(t0)
    sd a2, 16(t0)
    sd a3, 24(t0)
    sd a7, 32(t0)
    sd t1, 40(t0)
    sd t2, 48(t0)
    ret
    .option pop
#endif
    .section .note.GNU-stack,""
