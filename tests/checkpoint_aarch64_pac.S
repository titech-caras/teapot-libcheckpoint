#include "checkpoint.h"

	.text
	.globl checkpoint_pac_probe
	.type checkpoint_pac_probe,%function
/* int checkpoint_pac_probe(void *stack_top, void *target);
 * Creates a real checkpoint, mutates runtime state, then transfers to target.
 * An authentication fault must roll back; the probe returns 0 if it returned
 * instead of rolling back, 42 after a verified rollback, 1 on state mismatch. */
checkpoint_pac_probe:
	mov x9, sp
	mov sp, x0
	stp x9, x30, [sp, #-16]!
	str x1, [sp, #-16]!
	sub x9, sp, #(AARCH64_SHADOW_STACK_SIZE >> 12), lsl #12
	mov x16, #0x1240
	mov x17, #0x1340
	stp x16, x17, [x9, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	mov x2, #0x2240
	ldr x17, =checkpoint_target_metadata
	adr x16, .Lpac_selected
	str x16, [x17, #CHECKPOINT_TARGET_TRAMPOLINE_ADDR]
	adr x16, .Lpac_restored
	str x16, [x17, #CHECKPOINT_TARGET_RETURN_ADDR]
	b make_checkpoint_aarch64
.Lpac_selected:
	bl checkpoint_pac_mutate
	ldr x0, [sp]
	blr x0
	/* The failing authentication must fault and roll back, never return here. */
	mov w0, #0
	b .Lpac_return
.Lpac_restored:
	ldr x16, =checkpoint_target_metadata
	ldr x16, [x16, #CHECKPOINT_TARGET_SCRATCH_REG_ADDR]
	mov x0, #0x1240
	cmp x16, x0
	b.ne .Lpac_failed
	mov x0, #0x1340
	cmp x17, x0
	b.ne .Lpac_failed
	mov x0, #0x2240
	cmp x2, x0
	b.ne .Lpac_failed
	mov w0, #42
	b .Lpac_return
.Lpac_failed:
	mov w0, #1
.Lpac_return:
	add sp, sp, #16
	ldp x9, x30, [sp], #16
	mov sp, x9
	ret
	.size checkpoint_pac_probe, .-checkpoint_pac_probe

	.globl teapot_pac_bad_auth
	.type teapot_pac_bad_auth,%function
/* Signs a pointer, corrupts one PAC bit, then authenticates it. FEAT_FPAC
 * raises SIGILL at the AUTIA; without FPAC the returned pointer is poisoned.
 * GNU as needs no pauth extension for explicit encodings. */
teapot_pac_bad_auth:
	mov x0, #0x89ab
	movk x0, #0x4567, lsl #16
	movk x0, #0x1234, lsl #32
	.inst 0xdac103e0			/* pacia x0, sp */
	eor x0, x0, #0x0040000000000000	/* flip PAC bit 54 (see fpac_probe_child) */
	.inst 0xdac113e0			/* autia x0, sp */
	ret
	.size teapot_pac_bad_auth, .-teapot_pac_bad_auth
	.section .note.GNU-stack,"",%progbits
