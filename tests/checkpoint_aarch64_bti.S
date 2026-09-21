#include "checkpoint.h"

	.text
	.globl checkpoint_bti_probe
	.type checkpoint_bti_probe,%function
checkpoint_bti_probe:
	mov x9, sp
	mov sp, x0
	stp x9, x30, [sp, #-16]!
	str x1, [sp, #-16]!
	sub x9, sp, #(AARCH64_SHADOW_STACK_SIZE >> 12), lsl #12
	mov x16, #0x1640
	mov x17, #0x1740
	stp x16, x17, [x9, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	mov x2, #0x2240
	ldr x17, =checkpoint_target_metadata
	adr x16, .Lbti_selected
	str x16, [x17, #CHECKPOINT_TARGET_TRAMPOLINE_ADDR]
	adr x16, .Lbti_restored
	str x16, [x17, #CHECKPOINT_TARGET_RETURN_ADDR]
	b make_checkpoint_aarch64
.Lbti_selected:
	bl checkpoint_bti_mutate
	ldr x0, [sp]
	blr x0
	/* The invalid target must fault and roll back, never return here. */
	mov w0, #0
	b .Lbti_return
.Lbti_restored:
	/* Match the real rewriter's return landing, including deferred x16. */
	ldr x16, =checkpoint_target_metadata
	ldr x16, [x16, #CHECKPOINT_TARGET_SCRATCH_REG_ADDR]
	mov x0, #0x1640
	cmp x16, x0
	b.ne .Lbti_failed
	mov x0, #0x1740
	cmp x17, x0
	b.ne .Lbti_failed
	mov x0, #0x2240
	cmp x2, x0
	b.ne .Lbti_failed
	mov w0, #42
	b .Lbti_return
.Lbti_failed:
	mov w0, #1
.Lbti_return:
	add sp, sp, #16
	ldp x9, x30, [sp], #16
	mov sp, x9
	ret
	.size checkpoint_bti_probe, .-checkpoint_bti_probe

#ifdef ENABLE_NESTED_SPECULATION
	.globl checkpoint_bti_nested_probe
	.type checkpoint_bti_nested_probe,%function
checkpoint_bti_nested_probe:
	/* Keep the caller frame intact while two real checkpoint slots are live. */
	mov x9, sp
	mov sp, x0
	stp x9, x30, [sp, #-16]!
	str x1, [sp, #-16]!
	sub x9, sp, #(AARCH64_SHADOW_STACK_SIZE >> 12), lsl #12
	mov x16, #0x1650
	mov x17, #0x1750
	stp x16, x17, [x9, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	mov x2, #0x2250
	ldr x17, =checkpoint_target_metadata
	adr x16, .Lchain_selected
	str x16, [x17, #CHECKPOINT_TARGET_TRAMPOLINE_ADDR]
	adr x16, .Lchain_restored
	str x16, [x17, #CHECKPOINT_TARGET_RETURN_ADDR]
	b make_checkpoint_aarch64
.Lchain_selected:
	bl checkpoint_bti_outer_mutate
	/* The inner entry saves its own caller frame below the outer saved frame. */
	sub x0, sp, #4096
	ldr x1, [sp]
	bl checkpoint_bti_probe
	bl checkpoint_bti_check_outer
	b restore_checkpoint_ROB_LEN
.Lchain_restored:
	ldr x16, =checkpoint_target_metadata
	ldr x16, [x16, #CHECKPOINT_TARGET_SCRATCH_REG_ADDR]
	mov x0, #0x1650
	cmp x16, x0
	b.ne .Lchain_failed
	mov x0, #0x1750
	cmp x17, x0
	b.ne .Lchain_failed
	mov x0, #0x2250
	cmp x2, x0
	b.ne .Lchain_failed
	mov w0, #42
	b .Lchain_return
.Lchain_failed:
	mov w0, #1
.Lchain_return:
	add sp, sp, #16
	ldp x9, x30, [sp], #16
	mov sp, x9
	ret
	.size checkpoint_bti_nested_probe, .-checkpoint_bti_nested_probe
#endif
	.section .note.GNU-stack,"",%progbits
