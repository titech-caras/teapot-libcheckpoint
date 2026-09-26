#include "checkpoint.h"

	.text
	.globl	checkpoint_entry_probe
#if defined(__x86_64__)
	.type	checkpoint_entry_probe,@function
checkpoint_entry_probe:
	leaq	.Lselected(%rip), %rax
	movq	%rax, checkpoint_target_metadata+CHECKPOINT_TARGET_TRAMPOLINE_ADDR(%rip)
	leaq	.Lskipped(%rip), %rax
	movq	%rax, checkpoint_target_metadata+CHECKPOINT_TARGET_RETURN_ADDR(%rip)
	jmp	make_checkpoint_x64
.Lselected:
	movl	$1, %eax
	retq
.Lskipped:
	xorl	%eax, %eax
	retq
#elif defined(__aarch64__)
	.type	checkpoint_entry_probe,%function
checkpoint_entry_probe:
	/* The lower half of the test mapping backs the fixed-offset spills. */
	mov	x9, sp
	mov	sp, x0
	stp	x9, x30, [sp, #-16]!
	sub	x9, sp, #(AARCH64_SHADOW_STACK_SIZE >> 12), lsl #12
	mov	x16, #0x1640
	mov	x17, #0x1740
	stp	x16, x17, [x9, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	ldr	x17, =checkpoint_target_metadata
	adr	x16, .Lselected
	str	x16, [x17, #CHECKPOINT_TARGET_TRAMPOLINE_ADDR]
	adr	x16, .Lskipped
	str	x16, [x17, #CHECKPOINT_TARGET_RETURN_ADDR]
	b	make_checkpoint_aarch64
.Lselected:
	mov	w0, #1
	b	.Lreturn
.Lskipped:
	mov	w0, #0
.Lreturn:
	ldp	x9, x30, [sp], #16
	mov	sp, x9
	ret
#elif defined(__riscv) && __riscv_xlen == 64
	.option	push
	.option	norelax
	.type	checkpoint_entry_probe,@function
checkpoint_entry_probe:
	/* This ABI test entry has a free a1 for the first-spill base. */
	lla	a1, scratchpad + SCRATCHPAD_FIRST_SPILL_OFFSET
	sd	t0, 0(a1)
	sd	t1, 8(a1)
	lla	t1, checkpoint_target_metadata
	lla	t0, .Lselected
	sd	t0, CHECKPOINT_TARGET_TRAMPOLINE_ADDR(t1)
	lla	t0, .Lskipped
	sd	t0, CHECKPOINT_TARGET_RETURN_ADDR(t1)
	tail	make_checkpoint_riscv64
.Lselected:
	li	a0, 1
	ret
.Lskipped:
	li	a0, 0
	ret
	.option	pop
#endif
	.size	checkpoint_entry_probe, .-checkpoint_entry_probe

	.globl	checkpoint_rollback_probe
#if defined(__x86_64__)
	.type	checkpoint_rollback_probe,@function
checkpoint_rollback_probe:
	leaq	checkpoint_test_transient_body(%rip), %rax
	movq	%rax, checkpoint_target_metadata+CHECKPOINT_TARGET_TRAMPOLINE_ADDR(%rip)
	leaq	.Lqueue_restored(%rip), %rax
	movq	%rax, checkpoint_target_metadata+CHECKPOINT_TARGET_RETURN_ADDR(%rip)
	jmp	make_checkpoint_x64
.Lqueue_restored:
	movl	$1, %eax
	retq
#elif defined(__aarch64__)
	.type	checkpoint_rollback_probe,%function
checkpoint_rollback_probe:
	mov	x9, sp
	mov	sp, x0
	stp	x9, x30, [sp, #-16]!
	sub	x9, sp, #(AARCH64_SHADOW_STACK_SIZE >> 12), lsl #12
	stp	x16, x17, [x9, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	ldr	x17, =checkpoint_target_metadata
	adr	x16, checkpoint_test_transient_body
	str	x16, [x17, #CHECKPOINT_TARGET_TRAMPOLINE_ADDR]
	adr	x16, .Lqueue_restored
	str	x16, [x17, #CHECKPOINT_TARGET_RETURN_ADDR]
	b	make_checkpoint_aarch64
.Lqueue_restored:
	ldp	x9, x30, [sp], #16
	mov	sp, x9
	mov	w0, #1
	ret
#elif defined(__riscv) && __riscv_xlen == 64
	.option	push
	.option	norelax
	.type	checkpoint_rollback_probe,@function
checkpoint_rollback_probe:
	lla	a1, scratchpad + SCRATCHPAD_FIRST_SPILL_OFFSET
	sd	t0, 0(a1)
	sd	t1, 8(a1)
	lla	t1, checkpoint_target_metadata
	lla	t0, checkpoint_test_transient_body
	sd	t0, CHECKPOINT_TARGET_TRAMPOLINE_ADDR(t1)
	lla	t0, .Lqueue_restored
	sd	t0, CHECKPOINT_TARGET_RETURN_ADDR(t1)
	tail	make_checkpoint_riscv64
.Lqueue_restored:
	li	a0, 1
	ret
	.option	pop
#endif
	.size	checkpoint_rollback_probe, .-checkpoint_rollback_probe

#if defined(__aarch64__)
	.globl	checkpoint_report_probe
	.type	checkpoint_report_probe,%function
checkpoint_report_probe:
	mov	x9, sp
	mov	sp, x0
	stp	x9, x30, [sp, #-16]!
	ldr	x16, =scratchpad + SCRATCHPAD_SIZE - 512
	adr	x17, .Lreport_site
	str	x17, [x16]
	str	xzr, [x16, #8]
	str	xzr, [x16, #16]
	mov	x16, #0x1640
	mov	x17, #0x1740
	bl	report_gadget_aarch64_preserve_KASPER_CACHE
	mov	w0, #0
	mov	x9, #0x1640
	cmp	x16, x9
	b.ne	.Lreport_return
	mov	x9, #0x1740
	cmp	x17, x9
	b.ne	.Lreport_return
	mov	w0, #1
.Lreport_return:
	ldp	x9, x30, [sp], #16
	mov	sp, x9
	ret
.Lreport_site:
	nop
	.size	checkpoint_report_probe, .-checkpoint_report_probe
#endif
	.section	.note.GNU-stack,""
