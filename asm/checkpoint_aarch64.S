#include "checkpoint.h"

// AArch64 indirect branches require a register. The final transfer uses x16
// (IP0), so the architecture-specific instrumentation must reserve it there.
#define AARCH64_FRAME_SIZE 64
#define AARCH64_REPORT_STATE_OFFSET (SCRATCHPAD_SIZE - 512)
#define AARCH64_REPORT_CALL_STACK_OFFSET (SCRATCHPAD_SIZE - 4096)
#ifndef AARCH64_SHADOW_STACK_CONTROL_OFFSET
#define AARCH64_SHADOW_STACK_CONTROL_OFFSET 320
#endif

.macro LOAD_ADDR reg, sym
	ldr	\reg, =\sym
.endm

.macro READ_TIMER_STAT result, addr, prev
	mrs	\result, cntvct_el0
	LOAD_ADDR \addr, last_rdtsc
	ldr	\prev, [\addr]
	sub	\prev, \result, \prev
	str	\result, [\addr]
	mov	\result, \prev
.endm

.macro SAVE_SIMD_STATE base
#ifdef ENABLE_AARCH64_SIMD_STATE
	stp	q0, q1, [\base, #0]
	stp	q2, q3, [\base, #32]
	stp	q4, q5, [\base, #64]
	stp	q6, q7, [\base, #96]
	stp	q8, q9, [\base, #128]
	stp	q10, q11, [\base, #160]
	stp	q12, q13, [\base, #192]
	stp	q14, q15, [\base, #224]
	stp	q16, q17, [\base, #256]
	stp	q18, q19, [\base, #288]
	stp	q20, q21, [\base, #320]
	stp	q22, q23, [\base, #352]
	stp	q24, q25, [\base, #384]
	stp	q26, q27, [\base, #416]
	stp	q28, q29, [\base, #448]
	stp	q30, q31, [\base, #480]
#endif
.endm

.macro RESTORE_SIMD_STATE base
#ifdef ENABLE_AARCH64_SIMD_STATE
	ldp	q0, q1, [\base, #0]
	ldp	q2, q3, [\base, #32]
	ldp	q4, q5, [\base, #64]
	ldp	q6, q7, [\base, #96]
	ldp	q8, q9, [\base, #128]
	ldp	q10, q11, [\base, #160]
	ldp	q12, q13, [\base, #192]
	ldp	q14, q15, [\base, #224]
	ldp	q16, q17, [\base, #256]
	ldp	q18, q19, [\base, #288]
	ldp	q20, q21, [\base, #320]
	ldp	q22, q23, [\base, #352]
	ldp	q24, q25, [\base, #384]
	ldp	q26, q27, [\base, #416]
	ldp	q28, q29, [\base, #448]
	ldp	q30, q31, [\base, #480]
#endif
.endm

.macro SAVE_BOOTSTRAP_REGS_FROM_SHADOW shadow
	stp	x0, x1, [sp, #0]
	mov	x0, \shadow
	ldr	x16, [x0, #AARCH64_SHADOW_STACK_CONTROL_OFFSET]
	ldr	x17, [x0, #(AARCH64_SHADOW_STACK_CONTROL_OFFSET + 8)]
	stp	x16, x17, [sp, #16]
	str	x19, [sp, #32]
	LOAD_ADDR x16, old_rsp
	ldr	x17, [x16]
	str	x17, [sp, #40]
	mrs	x17, nzcv
	str	x17, [sp, #48]
.endm

.macro STORE_REGS_TO_CHECKPOINT ckpt
	ldp	x0, x1, [sp, #0]
	stp	x0, x1, [\ckpt, #CKPT_AARCH64_X0]
	stp	x2, x3, [\ckpt, #CKPT_AARCH64_X2]
	stp	x4, x5, [\ckpt, #CKPT_AARCH64_X4]
	stp	x6, x7, [\ckpt, #CKPT_AARCH64_X6]
	stp	x8, x9, [\ckpt, #CKPT_AARCH64_X8]
	stp	x10, x11, [\ckpt, #CKPT_AARCH64_X10]
	stp	x12, x13, [\ckpt, #CKPT_AARCH64_X12]
	stp	x14, x15, [\ckpt, #CKPT_AARCH64_X14]
	ldp	x0, x1, [sp, #16]
	stp	x0, x1, [\ckpt, #CKPT_AARCH64_X16]
	str	x18, [\ckpt, #CKPT_AARCH64_X18]
	ldr	x0, [sp, #32]
	str	x0, [\ckpt, #CKPT_AARCH64_X19]
	stp	x20, x21, [\ckpt, #CKPT_AARCH64_X20]
	stp	x22, x23, [\ckpt, #CKPT_AARCH64_X22]
	stp	x24, x25, [\ckpt, #CKPT_AARCH64_X24]
	stp	x26, x27, [\ckpt, #CKPT_AARCH64_X26]
	stp	x28, x29, [\ckpt, #CKPT_AARCH64_X28]
	str	x30, [\ckpt, #CKPT_AARCH64_X30]
	ldr	x0, [sp, #40]
	str	x0, [\ckpt, #CKPT_AARCH64_SP]
	ldr	x0, [sp, #48]
	str	x0, [\ckpt, #CKPT_AARCH64_NZCV]
.endm

.macro COPY_DIFT_TAGS ckpt
#ifdef ENABLE_DIFT
	LOAD_ADDR x16, dift_reg_tags
	ldp	x0, x1, [x16, #0]
	stp	x0, x1, [\ckpt, #CKPT_DIFT_REG_TAGS]
	ldp	x0, x1, [x16, #16]
	stp	x0, x1, [\ckpt, #CKPT_DIFT_REG_TAGS + 16]
	ldp	x0, x1, [x16, #32]
	stp	x0, x1, [\ckpt, #CKPT_DIFT_REG_TAGS + 32]
#endif
.endm

.macro RESTORE_FROM_TEMP target_offset
	mov	x16, sp
	ldr	x17, [x16, #16]
	LOAD_ADDR x0, checkpoint_target_metadata + CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	str	x17, [x0]
	ldr	x17, [x16, #40]
	mov	sp, x17
	ldr	x17, [x16, #48]
	msr	nzcv, x17
	ldp	x0, x1, [x16, #0]
	ldr	x19, [x16, #32]
	ldr	x17, [x16, #24]
	LOAD_ADDR x16, checkpoint_target_metadata + \target_offset
	ldr	x16, [x16]
	br	x16
.endm

.macro RESTORE_FROM_CHECKPOINT_TARGET ckpt, target_offset
	ldr	x17, [\ckpt, #CKPT_AARCH64_X16]
	LOAD_ADDR x0, checkpoint_target_metadata + CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	str	x17, [x0]
	ldr	x17, [\ckpt, #CKPT_AARCH64_SP]
	mov	sp, x17
	ldr	x17, [\ckpt, #CKPT_AARCH64_NZCV]
	msr	nzcv, x17
	ldp	x0, x1, [\ckpt, #CKPT_AARCH64_X0]
	ldp	x2, x3, [\ckpt, #CKPT_AARCH64_X2]
	ldp	x4, x5, [\ckpt, #CKPT_AARCH64_X4]
	ldp	x6, x7, [\ckpt, #CKPT_AARCH64_X6]
	ldp	x8, x9, [\ckpt, #CKPT_AARCH64_X8]
	ldp	x10, x11, [\ckpt, #CKPT_AARCH64_X10]
	ldp	x12, x13, [\ckpt, #CKPT_AARCH64_X12]
	ldp	x14, x15, [\ckpt, #CKPT_AARCH64_X14]
	ldr	x18, [\ckpt, #CKPT_AARCH64_X18]
	ldp	x20, x21, [\ckpt, #CKPT_AARCH64_X20]
	ldp	x22, x23, [\ckpt, #CKPT_AARCH64_X22]
	ldp	x24, x25, [\ckpt, #CKPT_AARCH64_X24]
	ldp	x26, x27, [\ckpt, #CKPT_AARCH64_X26]
	ldp	x28, x29, [\ckpt, #CKPT_AARCH64_X28]
	ldr	x30, [\ckpt, #CKPT_AARCH64_X30]
	ldr	x17, [\ckpt, #CKPT_AARCH64_X17]
	ldr	x19, [\ckpt, #CKPT_AARCH64_X19]
	LOAD_ADDR x16, checkpoint_target_metadata + \target_offset
	ldr	x16, [x16]
	br	x16
.endm

.macro RESTORE_FROM_CHECKPOINT ckpt
	ldr	x17, [\ckpt, #CKPT_AARCH64_X16]
	LOAD_ADDR x0, checkpoint_target_metadata + CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	str	x17, [x0]
	ldr	x17, [\ckpt, #CKPT_AARCH64_SP]
	mov	sp, x17
	ldr	x17, [\ckpt, #CKPT_AARCH64_NZCV]
	msr	nzcv, x17
	ldp	x0, x1, [\ckpt, #CKPT_AARCH64_X0]
	ldp	x2, x3, [\ckpt, #CKPT_AARCH64_X2]
	ldp	x4, x5, [\ckpt, #CKPT_AARCH64_X4]
	ldp	x6, x7, [\ckpt, #CKPT_AARCH64_X6]
	ldp	x8, x9, [\ckpt, #CKPT_AARCH64_X8]
	ldp	x10, x11, [\ckpt, #CKPT_AARCH64_X10]
	ldp	x12, x13, [\ckpt, #CKPT_AARCH64_X12]
	ldp	x14, x15, [\ckpt, #CKPT_AARCH64_X14]
	ldr	x17, [\ckpt, #CKPT_AARCH64_X17]
	ldp	x18, x19, [\ckpt, #CKPT_AARCH64_X18]
	ldp	x20, x21, [\ckpt, #CKPT_AARCH64_X20]
	ldp	x22, x23, [\ckpt, #CKPT_AARCH64_X22]
	ldp	x24, x25, [\ckpt, #CKPT_AARCH64_X24]
	ldp	x26, x27, [\ckpt, #CKPT_AARCH64_X26]
	ldp	x28, x29, [\ckpt, #CKPT_AARCH64_X28]
	ldr	x30, [\ckpt, #CKPT_AARCH64_X30]
	ldr	x16, [\ckpt, #CKPT_RETURN_ADDRESS]
	br	x16
.endm

.macro DEF_RESTORE_CHECKPOINT name, reason
	.globl	restore_checkpoint_\name
	.p2align	4
	.type	restore_checkpoint_\name,%function
restore_checkpoint_\name:
	.cfi_startproc
	LOAD_ADDR x16, old_rsp
	mov	x17, sp
	str	x17, [x16]
	LOAD_ADDR x16, scratchpad + SCRATCHPAD_SIZE
	mov	sp, x16
	mov	x0, #\reason
	b	restore_checkpoint
.Lrestore_checkpoint_\name\()_end:
	.size	restore_checkpoint_\name, .Lrestore_checkpoint_\name\()_end-restore_checkpoint_\name
	.cfi_endproc
.endm

	.text
DEF_RESTORE_CHECKPOINT ROB_LEN, ROLLBACK_ROB_LEN
DEF_RESTORE_CHECKPOINT SIGSEGV, ROLLBACK_SIGSEGV
DEF_RESTORE_CHECKPOINT EXT_LIB, ROLLBACK_EXT_LIB
DEF_RESTORE_CHECKPOINT MALFORMED_INDIRECT_BR, ROLLBACK_MALFORMED_INDIRECT_BR

.macro DEF_REPORT_GADGET_PRESERVE type
	.globl	report_gadget_aarch64_preserve_\type
	.p2align	4
	.type	report_gadget_aarch64_preserve_\type,%function
report_gadget_aarch64_preserve_\type:
	.cfi_startproc
	LOAD_ADDR x16, scratchpad + AARCH64_REPORT_STATE_OFFSET
	stp	x0, x1, [x16, #32]
	stp	x2, x3, [x16, #48]
	stp	x4, x5, [x16, #64]
	stp	x6, x7, [x16, #80]
	stp	x8, x9, [x16, #96]
	stp	x10, x11, [x16, #112]
	stp	x12, x13, [x16, #128]
	stp	x14, x15, [x16, #144]
	stp	x17, x18, [x16, #160]
	str	x30, [x16, #176]
	mov	x17, sp
	str	x17, [x16, #184]
	mrs	x17, nzcv
	str	x17, [x16, #192]

	ldr	x0, [x16, #0]
	ldr	x1, [x16, #8]
	ldr	x2, [x16, #16]
	LOAD_ADDR x17, scratchpad + AARCH64_REPORT_CALL_STACK_OFFSET
	mov	sp, x17
	bl	report_gadget_\type

	LOAD_ADDR x16, scratchpad + AARCH64_REPORT_STATE_OFFSET
	ldr	x17, [x16, #192]
	msr	nzcv, x17
	ldr	x17, [x16, #184]
	mov	sp, x17
	ldr	x30, [x16, #176]
	ldp	x17, x18, [x16, #160]
	ldp	x14, x15, [x16, #144]
	ldp	x12, x13, [x16, #128]
	ldp	x10, x11, [x16, #112]
	ldp	x8, x9, [x16, #96]
	ldp	x6, x7, [x16, #80]
	ldp	x4, x5, [x16, #64]
	ldp	x2, x3, [x16, #48]
	ldp	x0, x1, [x16, #32]
	ret
.Lreport_gadget_aarch64_preserve_\type\()_end:
	.size	report_gadget_aarch64_preserve_\type, .Lreport_gadget_aarch64_preserve_\type\()_end-report_gadget_aarch64_preserve_\type
	.cfi_endproc
.endm

DEF_REPORT_GADGET_PRESERVE KASPER_CACHE
DEF_REPORT_GADGET_PRESERVE KASPER_MDS
DEF_REPORT_GADGET_PRESERVE KASPER_PORT

	.globl	make_checkpoint_aarch64
	.p2align	4
	.type	make_checkpoint_aarch64,%function
make_checkpoint_aarch64:
	.cfi_startproc
	mov	x17, sp
	LOAD_ADDR x16, old_rsp
	str	x17, [x16]
	sub	x17, x17, #2048, lsl #12
	LOAD_ADDR x16, scratchpad + SCRATCHPAD_SIZE
	mov	sp, x16
	sub	sp, sp, #AARCH64_FRAME_SIZE
	SAVE_BOOTSTRAP_REGS_FROM_SHADOW x17

.Lmake_checkpoint_after_save_temp:
	LOAD_ADDR x16, libcheckpoint_enabled
	ldrb	w16, [x16]
	cbz	w16, .Lskip_checkpoint

	LOAD_ADDR x16, checkpoint_cnt
	ldr	x0, [x16]
	cbnz	x0, .Lif_should_checkpoint

#ifdef USE_BRANCH_EXEC_COUNT
	LOAD_ADDR x16, checkpoint_target_metadata + CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR
	ldr	x16, [x16]
	ldr	w1, [x16]
	add	w1, w1, #1
	str	w1, [x16]
#endif

#ifdef BRANCH_FULL_EXEC_COUNT
	cmp	x1, #BRANCH_FULL_EXEC_COUNT
	b.gt	.Lheuristic
	LOAD_ADDR x16, max_checkpoints
	mov	x17, #MAX_CHECKPOINTS
	str	x17, [x16]
	b	.Lif_should_checkpoint
#endif

.Lheuristic:
#ifdef SPECFUZZ_PRIORITIZED_SIMULATION
	rbit	x1, x1
	clz	x1, x1
	lsr	x1, x1, #1
	add	x1, x1, #1
	LOAD_ADDR x16, max_checkpoints
	str	x1, [x16]
#else
	LOAD_ADDR x16, max_checkpoints
	mov	x17, #1
	str	x17, [x16]
#endif

.Lif_should_checkpoint:
	LOAD_ADDR x16, max_checkpoints
	ldr	x17, [x16]
	cmp	x0, x17
	b.ge	.Lskip_checkpoint
	LOAD_ADDR x16, checkpoint_cnt
	add	x17, x0, #1
	str	x17, [x16]

	LOAD_ADDR x16, processor_extended_states
	add	x16, x16, x0, lsl #PROCESSOR_EXTENDED_STATE_SHIFT

	SAVE_SIMD_STATE x16

	LOAD_ADDR x19, checkpoint_metadata
	add	x19, x19, x0, lsl #CHECKPOINT_METADATA_SHIFT
	STORE_REGS_TO_CHECKPOINT x19

#ifdef TIME
	READ_TIMER_STAT x1, x2, x3
	mov	x2, #0
	cmp	x17, #1
	cset	x2, gt
	LOAD_ADDR x3, simulation_statistics
	add	x3, x3, x2, lsl #3
	ldr	x4, [x3]
	add	x4, x4, x1
	str	x4, [x3]
#endif

	COPY_DIFT_TAGS x19

	LOAD_ADDR x16, checkpoint_target_metadata + CHECKPOINT_TARGET_RETURN_ADDR
	ldr	x17, [x16]
	str	x17, [x19, #CKPT_RETURN_ADDRESS]
	LOAD_ADDR x16, instruction_cnt
	ldr	x17, [x16]
	str	x17, [x19, #CKPT_INSTRUCTION_CNT]
	LOAD_ADDR x16, memory_history_top
	ldr	x17, [x16]
	str	x17, [x19, #CKPT_MEMORY_HISTORY_TOP]
	LOAD_ADDR x16, guard_list_top
	ldr	x17, [x16]
	str	x17, [x19, #CKPT_GUARD_LIST_TOP]

#ifdef COVERAGE
	LOAD_ADDR x16, checkpoint_cnt
	ldr	x16, [x16]
	cmp	x16, #1
	b.gt	.Lno_normal_coverage
	ldr	x0, [x19, #CKPT_RETURN_ADDRESS]
	bl	hfuzz_trace_pc
.Lno_normal_coverage:
#endif

#ifdef TIME
	READ_TIMER_STAT x1, x2, x3
	LOAD_ADDR x2, simulation_statistics + 16
	ldr	x3, [x2]
	add	x3, x3, x1
	str	x3, [x2]
#endif

	RESTORE_FROM_CHECKPOINT_TARGET x19, CHECKPOINT_TARGET_TRAMPOLINE_ADDR

.Lskip_checkpoint:
	RESTORE_FROM_TEMP CHECKPOINT_TARGET_RETURN_ADDR
.Lmake_checkpoint_end:
	.size	make_checkpoint_aarch64, .Lmake_checkpoint_end-make_checkpoint_aarch64
	.cfi_endproc

	.globl	restore_checkpoint_registers
	.p2align	4
	.type	restore_checkpoint_registers,%function
restore_checkpoint_registers:
	.cfi_startproc
	LOAD_ADDR x16, checkpoint_cnt
	ldr	x0, [x16]
	LOAD_ADDR x16, checkpoint_metadata
	add	x16, x16, x0, lsl #CHECKPOINT_METADATA_SHIFT

	LOAD_ADDR x1, processor_extended_states
	add	x1, x1, x0, lsl #PROCESSOR_EXTENDED_STATE_SHIFT
	RESTORE_SIMD_STATE x1

#ifdef TIME
	mov	x19, x16
	READ_TIMER_STAT x0, x1, x2
	LOAD_ADDR x1, simulation_statistics + 24
	ldr	x2, [x1]
	add	x2, x2, x0
	str	x2, [x1]
	mov	x16, x19
#endif

	ldr	x17, [x16, #CKPT_RETURN_ADDRESS]
	LOAD_ADDR x0, checkpoint_target_metadata + CHECKPOINT_TARGET_RETURN_ADDR
	str	x17, [x0]
	RESTORE_FROM_CHECKPOINT x16
.Lrestore_checkpoint_registers_end:
	.size	restore_checkpoint_registers, .Lrestore_checkpoint_registers_end-restore_checkpoint_registers
	.cfi_endproc

	.bss
	.globl	checkpoint_target_metadata
	.p2align	3, 0x0
	.type	checkpoint_target_metadata,%object
checkpoint_target_metadata:
	.zero	CHECKPOINT_TARGET_METADATA_SIZE
	.size	checkpoint_target_metadata, CHECKPOINT_TARGET_METADATA_SIZE

	.globl	max_checkpoints
	.p2align	3, 0x0
	.type	max_checkpoints,%object
max_checkpoints:
	.zero	8
	.size	max_checkpoints, 8

	.section	.note.GNU-stack,"",%progbits
