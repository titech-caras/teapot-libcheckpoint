/* A rollback restores the RISC-V floating-point registers and FCSR in a runtime
 * built with TEAPOT_ENABLE_RISCV_FLOAT_STATE: Teapot requires that capability
 * for every RISC-V rewrite with checkpoints. The speculative path changes f8,
 * f31 and FCSR and rolls back; the restored path records what it sees. */
#include "checkpoint.h"

	.text
	.option	push
	.option	norelax
	.globl	checkpoint_float_probe
	.type	checkpoint_float_probe,@function
checkpoint_float_probe:
	li	t2, 0x400921fb54442d18		/* pi */
	fmv.d.x	f8, t2
	li	t2, 0x4005bf0a8b145769		/* e */
	fmv.d.x	f31, t2
	li	t2, 0x23			/* frm 1 (round toward zero); fflags NX and UF */
	fscsr	t2
	/* The checkpoint entry protocol, as Teapot emits it. */
	lla	a1, scratchpad + SCRATCHPAD_FIRST_SPILL_OFFSET
	sd	t0, 0(a1)
	sd	t1, 8(a1)
	lla	t1, checkpoint_target_metadata
	lla	t0, .Lfloat_transient
	sd	t0, CHECKPOINT_TARGET_TRAMPOLINE_ADDR(t1)
	lla	t0, .Lfloat_restored
	sd	t0, CHECKPOINT_TARGET_RETURN_ADDR(t1)
	tail	make_checkpoint_riscv64
.Lfloat_transient:
	/* Plain stores are not logged: this one survives the rollback. */
	lla	t0, checkpoint_float_seen
	li	t1, 1
	sd	t1, 24(t0)
	fmv.d.x	f8, zero
	fmv.d.x	f31, zero
	fscsr	zero
	tail	restore_checkpoint_ROB_LEN
.Lfloat_restored:
	lla	t0, checkpoint_float_seen
	fmv.x.d	t1, f8
	sd	t1, 0(t0)
	fmv.x.d	t1, f31
	sd	t1, 8(t0)
	frcsr	t1
	sd	t1, 16(t0)
	ret
	.option	pop
	.size	checkpoint_float_probe, .-checkpoint_float_probe
	.section .note.GNU-stack,"",@progbits
