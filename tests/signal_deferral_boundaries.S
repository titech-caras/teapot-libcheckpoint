#include "checkpoint.h"
#include <asm/unistd.h>
/* Inject a real signal in windows that a periodic timer only hits by chance.
 * These wrappers exist only in this fixture, leaving runtime code untouched. */
#if defined(__x86_64__)
#define ENTRY make_checkpoint_x64
#elif defined(__aarch64__)
#define ENTRY make_checkpoint_aarch64
#else
#define ENTRY make_checkpoint_riscv64
#endif
#define WRAP2(x) __wrap_ ## x
#define WRAP(x) WRAP2(x)
#define REAL2(x) __real_ ## x
#define REAL(x) REAL2(x)
.macro INJECT flag
#if defined(__x86_64__)
    cmpl $0, \flag(%rip)
    je 1f
    movl $0, \flag(%rip)
    movl test_signal_pid(%rip), %edi
    movl %edi, %esi
    movl $10, %edx /* SIGUSR1 */
    movl $__NR_tgkill, %eax
    syscall
#elif defined(__aarch64__)
    ldr x16, =\flag
    ldr w17, [x16]
    cbz w17, 1f
    str wzr, [x16]
    ldr x16, =test_signal_pid
    ldr w0, [x16]
    mov x1, x0
    mov x2, #10
    mov x8, #__NR_tgkill
    svc #0
#else
    lla t0, \flag
    lw t1, 0(t0)
    beqz t1, 1f
    sw zero, 0(t0)
    lla t0, test_signal_pid
    lw a0, 0(t0)
    mv a1, a0
    li a2, 10
    li a7, __NR_tgkill
    ecall
#endif
1:
.endm
    .section teapot_checkpoint,"ax"
    .globl WRAP(ENTRY)
WRAP(ENTRY):
    INJECT test_signal_at_entry
#if defined(__x86_64__)
    jmp REAL(ENTRY)
#elif defined(__aarch64__)
    b REAL(ENTRY)
#else
    tail REAL(ENTRY)
#endif
    .section teapot_restore,"ax"
    .globl __wrap_restore_checkpoint_registers
__wrap_restore_checkpoint_registers:
    INJECT test_signal_at_restore
#if defined(__x86_64__)
    jmp __real_restore_checkpoint_registers
#elif defined(__aarch64__)
    b __real_restore_checkpoint_registers
#else
    tail __real_restore_checkpoint_registers
#endif
    .section .note.GNU-stack,""
