#include "checkpoint.h"

// RISC-V indirect branches require a register. The final transfer uses t0/x5;
// architecture-specific landing pads restore it before instrumented code resumes.
#define RISCV_FRAME_SIZE 80
#define RISCV_BOOT_X5 0
#define RISCV_BOOT_X6 8
#define RISCV_BOOT_X7 16
#define RISCV_BOOT_X8 24
#define RISCV_BOOT_X28 32
#define RISCV_BOOT_X29 40
#define RISCV_BOOT_X30 48
#define RISCV_BOOT_X31 56
#define RISCV_BOOT_SP 64

.macro LOAD_ADDR reg, symbol
	lla	\reg, \symbol
.endm

.macro LOAD_ADDR_OFFSET reg, symbol, offset
	LOAD_ADDR \reg, \symbol
	addi	\reg, \reg, \offset
.endm

.macro RDCYCLE_STAT result, addr, prev
	rdcycle	\result
	LOAD_ADDR \addr, last_rdtsc
	ld	\prev, 0(\addr)
	sub	\prev, \result, \prev
	sd	\result, 0(\addr)
	mv	\result, \prev
.endm

.macro SAVE_FLOAT_STATE base
#ifdef ENABLE_RISCV_FLOAT_STATE
	fsd	f0, 0(\base)
	fsd	f1, 8(\base)
	fsd	f2, 16(\base)
	fsd	f3, 24(\base)
	fsd	f4, 32(\base)
	fsd	f5, 40(\base)
	fsd	f6, 48(\base)
	fsd	f7, 56(\base)
	fsd	f8, 64(\base)
	fsd	f9, 72(\base)
	fsd	f10, 80(\base)
	fsd	f11, 88(\base)
	fsd	f12, 96(\base)
	fsd	f13, 104(\base)
	fsd	f14, 112(\base)
	fsd	f15, 120(\base)
	fsd	f16, 128(\base)
	fsd	f17, 136(\base)
	fsd	f18, 144(\base)
	fsd	f19, 152(\base)
	fsd	f20, 160(\base)
	fsd	f21, 168(\base)
	fsd	f22, 176(\base)
	fsd	f23, 184(\base)
	fsd	f24, 192(\base)
	fsd	f25, 200(\base)
	fsd	f26, 208(\base)
	fsd	f27, 216(\base)
	fsd	f28, 224(\base)
	fsd	f29, 232(\base)
	fsd	f30, 240(\base)
	fsd	f31, 248(\base)
#endif
.endm

.macro RESTORE_FLOAT_STATE base
#ifdef ENABLE_RISCV_FLOAT_STATE
	fld	f0, 0(\base)
	fld	f1, 8(\base)
	fld	f2, 16(\base)
	fld	f3, 24(\base)
	fld	f4, 32(\base)
	fld	f5, 40(\base)
	fld	f6, 48(\base)
	fld	f7, 56(\base)
	fld	f8, 64(\base)
	fld	f9, 72(\base)
	fld	f10, 80(\base)
	fld	f11, 88(\base)
	fld	f12, 96(\base)
	fld	f13, 104(\base)
	fld	f14, 112(\base)
	fld	f15, 120(\base)
	fld	f16, 128(\base)
	fld	f17, 136(\base)
	fld	f18, 144(\base)
	fld	f19, 152(\base)
	fld	f20, 160(\base)
	fld	f21, 168(\base)
	fld	f22, 176(\base)
	fld	f23, 184(\base)
	fld	f24, 192(\base)
	fld	f25, 200(\base)
	fld	f26, 208(\base)
	fld	f27, 216(\base)
	fld	f28, 224(\base)
	fld	f29, 232(\base)
	fld	f30, 240(\base)
	fld	f31, 248(\base)
#endif
.endm

.macro SAVE_BOOTSTRAP_REGS_FROM_FIRST_SPILL
	LOAD_ADDR t0, old_rsp
	ld	t0, 0(t0)
	sd	t0, RISCV_BOOT_SP(sp)
	LOAD_ADDR t0, scratchpad + SCRATCHPAD_FIRST_SPILL_OFFSET
	ld	t1, 0(t0)
	sd	t1, RISCV_BOOT_X5(sp)
	ld	t1, 8(t0)
	sd	t1, RISCV_BOOT_X6(sp)
	sd	t2, RISCV_BOOT_X7(sp)
	sd	s0, RISCV_BOOT_X8(sp)
	sd	t3, RISCV_BOOT_X28(sp)
	sd	t4, RISCV_BOOT_X29(sp)
	sd	t5, RISCV_BOOT_X30(sp)
	sd	t6, RISCV_BOOT_X31(sp)
.endm

.macro STORE_REGS_TO_CHECKPOINT ckpt
	sd	ra, CKPT_RISCV_X1(\ckpt)
	ld	t0, RISCV_BOOT_SP(sp)
	sd	t0, CKPT_RISCV_X2(\ckpt)
	sd	gp, CKPT_RISCV_X3(\ckpt)
	sd	tp, CKPT_RISCV_X4(\ckpt)
	ld	t0, RISCV_BOOT_X5(sp)
	sd	t0, CKPT_RISCV_X5(\ckpt)
	ld	t0, RISCV_BOOT_X6(sp)
	sd	t0, CKPT_RISCV_X6(\ckpt)
	ld	t0, RISCV_BOOT_X7(sp)
	sd	t0, CKPT_RISCV_X7(\ckpt)
	ld	t0, RISCV_BOOT_X8(sp)
	sd	t0, CKPT_RISCV_X8(\ckpt)
	sd	s1, CKPT_RISCV_X9(\ckpt)
	sd	a0, CKPT_RISCV_X10(\ckpt)
	sd	a1, CKPT_RISCV_X11(\ckpt)
	sd	a2, CKPT_RISCV_X12(\ckpt)
	sd	a3, CKPT_RISCV_X13(\ckpt)
	sd	a4, CKPT_RISCV_X14(\ckpt)
	sd	a5, CKPT_RISCV_X15(\ckpt)
	sd	a6, CKPT_RISCV_X16(\ckpt)
	sd	a7, CKPT_RISCV_X17(\ckpt)
	sd	s2, CKPT_RISCV_X18(\ckpt)
	sd	s3, CKPT_RISCV_X19(\ckpt)
	sd	s4, CKPT_RISCV_X20(\ckpt)
	sd	s5, CKPT_RISCV_X21(\ckpt)
	sd	s6, CKPT_RISCV_X22(\ckpt)
	sd	s7, CKPT_RISCV_X23(\ckpt)
	sd	s8, CKPT_RISCV_X24(\ckpt)
	sd	s9, CKPT_RISCV_X25(\ckpt)
	sd	s10, CKPT_RISCV_X26(\ckpt)
	sd	s11, CKPT_RISCV_X27(\ckpt)
	ld	t0, RISCV_BOOT_X28(sp)
	sd	t0, CKPT_RISCV_X28(\ckpt)
	ld	t0, RISCV_BOOT_X29(sp)
	sd	t0, CKPT_RISCV_X29(\ckpt)
	ld	t0, RISCV_BOOT_X30(sp)
	sd	t0, CKPT_RISCV_X30(\ckpt)
	ld	t0, RISCV_BOOT_X31(sp)
	sd	t0, CKPT_RISCV_X31(\ckpt)
.endm

.macro COPY_DIFT_TAGS ckpt
#ifdef ENABLE_DIFT
	LOAD_ADDR t1, dift_reg_tags
	ld	t2, 0(t1)
	sd	t2, CKPT_DIFT_REG_TAGS(\ckpt)
	ld	t2, 8(t1)
	sd	t2, CKPT_DIFT_REG_TAGS + 8(\ckpt)
	ld	t2, 16(t1)
	sd	t2, CKPT_DIFT_REG_TAGS + 16(\ckpt)
	ld	t2, 24(t1)
	sd	t2, CKPT_DIFT_REG_TAGS + 24(\ckpt)
	ld	t2, 32(t1)
	sd	t2, CKPT_DIFT_REG_TAGS + 32(\ckpt)
	ld	t2, 40(t1)
	sd	t2, CKPT_DIFT_REG_TAGS + 40(\ckpt)
#endif
.endm

.macro RESTORE_FROM_TEMP target_offset
	mv	t0, sp
	ld	t1, RISCV_BOOT_X5(t0)
	LOAD_ADDR_OFFSET t2, checkpoint_target_metadata, CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	sd	t1, 0(t2)
	ld	t1, RISCV_BOOT_SP(t0)
	mv	sp, t1
	ld	t1, RISCV_BOOT_X6(t0)
	ld	t2, RISCV_BOOT_X7(t0)
	ld	s0, RISCV_BOOT_X8(t0)
	ld	t3, RISCV_BOOT_X28(t0)
	ld	t4, RISCV_BOOT_X29(t0)
	ld	t5, RISCV_BOOT_X30(t0)
	ld	t6, RISCV_BOOT_X31(t0)
	LOAD_ADDR_OFFSET t0, checkpoint_target_metadata, \target_offset
	ld	t0, 0(t0)
	jr	t0
.endm

.macro RESTORE_FROM_CHECKPOINT_TARGET ckpt, target_offset
	ld	t1, CKPT_RISCV_X5(\ckpt)
	LOAD_ADDR_OFFSET t2, checkpoint_target_metadata, CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	sd	t1, 0(t2)
	ld	t1, CKPT_RISCV_X2(\ckpt)
	mv	sp, t1
	ld	ra, CKPT_RISCV_X1(\ckpt)
	ld	gp, CKPT_RISCV_X3(\ckpt)
	ld	tp, CKPT_RISCV_X4(\ckpt)
	ld	t0, CKPT_RISCV_X8(\ckpt)
	ld	t1, CKPT_RISCV_X6(\ckpt)
	ld	t2, CKPT_RISCV_X7(\ckpt)
	ld	s1, CKPT_RISCV_X9(\ckpt)
	ld	a0, CKPT_RISCV_X10(\ckpt)
	ld	a1, CKPT_RISCV_X11(\ckpt)
	ld	a2, CKPT_RISCV_X12(\ckpt)
	ld	a3, CKPT_RISCV_X13(\ckpt)
	ld	a4, CKPT_RISCV_X14(\ckpt)
	ld	a5, CKPT_RISCV_X15(\ckpt)
	ld	a6, CKPT_RISCV_X16(\ckpt)
	ld	a7, CKPT_RISCV_X17(\ckpt)
	ld	s2, CKPT_RISCV_X18(\ckpt)
	ld	s3, CKPT_RISCV_X19(\ckpt)
	ld	s4, CKPT_RISCV_X20(\ckpt)
	ld	s5, CKPT_RISCV_X21(\ckpt)
	ld	s6, CKPT_RISCV_X22(\ckpt)
	ld	s7, CKPT_RISCV_X23(\ckpt)
	ld	s8, CKPT_RISCV_X24(\ckpt)
	ld	s9, CKPT_RISCV_X25(\ckpt)
	ld	s10, CKPT_RISCV_X26(\ckpt)
	ld	s11, CKPT_RISCV_X27(\ckpt)
	ld	t3, CKPT_RISCV_X28(\ckpt)
	ld	t4, CKPT_RISCV_X29(\ckpt)
	ld	t5, CKPT_RISCV_X30(\ckpt)
	ld	t6, CKPT_RISCV_X31(\ckpt)
	mv	s0, t0
	LOAD_ADDR_OFFSET t0, checkpoint_target_metadata, \target_offset
	ld	t0, 0(t0)
	jr	t0
.endm

.macro RESTORE_FROM_CHECKPOINT ckpt
	ld	t1, CKPT_RISCV_X5(\ckpt)
	LOAD_ADDR_OFFSET t2, checkpoint_target_metadata, CHECKPOINT_TARGET_SCRATCH_REG_ADDR
	sd	t1, 0(t2)
	ld	t1, CKPT_RISCV_X2(\ckpt)
	mv	sp, t1
	ld	ra, CKPT_RISCV_X1(\ckpt)
	ld	gp, CKPT_RISCV_X3(\ckpt)
	ld	tp, CKPT_RISCV_X4(\ckpt)
	ld	t1, CKPT_RISCV_X6(\ckpt)
	ld	t2, CKPT_RISCV_X7(\ckpt)
	ld	s0, CKPT_RISCV_X8(\ckpt)
	ld	s1, CKPT_RISCV_X9(\ckpt)
	ld	a0, CKPT_RISCV_X10(\ckpt)
	ld	a1, CKPT_RISCV_X11(\ckpt)
	ld	a2, CKPT_RISCV_X12(\ckpt)
	ld	a3, CKPT_RISCV_X13(\ckpt)
	ld	a4, CKPT_RISCV_X14(\ckpt)
	ld	a5, CKPT_RISCV_X15(\ckpt)
	ld	a6, CKPT_RISCV_X16(\ckpt)
	ld	a7, CKPT_RISCV_X17(\ckpt)
	ld	s2, CKPT_RISCV_X18(\ckpt)
	ld	s3, CKPT_RISCV_X19(\ckpt)
	ld	s4, CKPT_RISCV_X20(\ckpt)
	ld	s5, CKPT_RISCV_X21(\ckpt)
	ld	s6, CKPT_RISCV_X22(\ckpt)
	ld	s7, CKPT_RISCV_X23(\ckpt)
	ld	s8, CKPT_RISCV_X24(\ckpt)
	ld	s9, CKPT_RISCV_X25(\ckpt)
	ld	s10, CKPT_RISCV_X26(\ckpt)
	ld	s11, CKPT_RISCV_X27(\ckpt)
	ld	t3, CKPT_RISCV_X28(\ckpt)
	ld	t4, CKPT_RISCV_X29(\ckpt)
	ld	t5, CKPT_RISCV_X30(\ckpt)
	ld	t6, CKPT_RISCV_X31(\ckpt)
	LOAD_ADDR_OFFSET t0, checkpoint_target_metadata, CHECKPOINT_TARGET_RETURN_ADDR
	ld	t0, 0(t0)
	jr	t0
.endm

.macro DEF_RESTORE_CHECKPOINT name, reason
	.globl	restore_checkpoint_\name
	.p2align	2
	.type	restore_checkpoint_\name,@function
restore_checkpoint_\name:
	.cfi_startproc
	LOAD_ADDR t0, old_rsp
	sd	sp, 0(t0)
	LOAD_ADDR sp, scratchpad
	li	t0, SCRATCHPAD_SIZE
	add	sp, sp, t0
	li	a0, \reason
	j	restore_checkpoint
.Lrestore_checkpoint_\name\()_end:
	.size	restore_checkpoint_\name, .Lrestore_checkpoint_\name\()_end-restore_checkpoint_\name
	.cfi_endproc
.endm

	.text
DEF_RESTORE_CHECKPOINT ROB_LEN, ROLLBACK_ROB_LEN
DEF_RESTORE_CHECKPOINT SIGSEGV, ROLLBACK_SIGSEGV
DEF_RESTORE_CHECKPOINT EXT_LIB, ROLLBACK_EXT_LIB
DEF_RESTORE_CHECKPOINT MALFORMED_INDIRECT_BR, ROLLBACK_MALFORMED_INDIRECT_BR

	.globl	make_checkpoint_riscv64
	.p2align	2
	.type	make_checkpoint_riscv64,@function
make_checkpoint_riscv64:
	.cfi_startproc
	LOAD_ADDR t0, old_rsp
	sd	sp, 0(t0)
	LOAD_ADDR t0, scratchpad
	li	t1, SCRATCHPAD_SIZE
	add	t0, t0, t1
	mv	sp, t0
	addi	sp, sp, -RISCV_FRAME_SIZE
	SAVE_BOOTSTRAP_REGS_FROM_FIRST_SPILL

.Lmake_checkpoint_after_save_temp:

	LOAD_ADDR t0, libcheckpoint_enabled
	lbu	t0, 0(t0)
	beqz	t0, .Lskip_checkpoint

	LOAD_ADDR t0, checkpoint_cnt
	ld	t2, 0(t0)
	bnez	t2, .Lif_should_checkpoint

#ifdef USE_BRANCH_EXEC_COUNT
	LOAD_ADDR_OFFSET t0, checkpoint_target_metadata, CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR
	ld	t0, 0(t0)
	lw	t3, 0(t0)
	addiw	t3, t3, 1
	sw	t3, 0(t0)
#endif

#ifdef BRANCH_FULL_EXEC_COUNT
	li	t4, BRANCH_FULL_EXEC_COUNT
	bgt	t3, t4, .Lheuristic
	LOAD_ADDR t0, max_checkpoints
	li	t1, MAX_CHECKPOINTS
	sd	t1, 0(t0)
	j	.Lif_should_checkpoint
#endif

.Lheuristic:
#ifdef SPECFUZZ_PRIORITIZED_SIMULATION
	mv	t4, t3
	li	t5, 0
.Lctz_loop:
	andi	t6, t4, 1
	bnez	t6, .Lctz_done
	addi	t5, t5, 1
	srli	t4, t4, 1
	j	.Lctz_loop
.Lctz_done:
	srli	t5, t5, 1
	addi	t5, t5, 1
	LOAD_ADDR t0, max_checkpoints
	sd	t5, 0(t0)
#else
	LOAD_ADDR t0, max_checkpoints
	li	t1, 1
	sd	t1, 0(t0)
#endif

.Lif_should_checkpoint:
	LOAD_ADDR t0, max_checkpoints
	ld	t1, 0(t0)
	bge	t2, t1, .Lskip_checkpoint
	LOAD_ADDR t0, checkpoint_cnt
	addi	t1, t2, 1
	sd	t1, 0(t0)

	LOAD_ADDR t0, processor_extended_states
	slli	t1, t2, PROCESSOR_EXTENDED_STATE_SHIFT
	add	t0, t0, t1

#ifdef TIME
	RDCYCLE_STAT t1, t3, t4
	LOAD_ADDR t3, checkpoint_cnt
	ld	t3, 0(t3)
	li	t4, 0
	li	t5, 1
	ble	t3, t5, .Lnormal_time
	li	t4, 1
.Lnormal_time:
	LOAD_ADDR t5, simulation_statistics
	slli	t4, t4, 3
	add	t5, t5, t4
	ld	t6, 0(t5)
	add	t6, t6, t1
	sd	t6, 0(t5)
#endif

	SAVE_FLOAT_STATE t0

	LOAD_ADDR t0, checkpoint_metadata
	slli	t1, t2, CHECKPOINT_METADATA_SHIFT
	add	s0, t0, t1
	STORE_REGS_TO_CHECKPOINT s0
	COPY_DIFT_TAGS s0

	LOAD_ADDR_OFFSET t0, checkpoint_target_metadata, CHECKPOINT_TARGET_RETURN_ADDR
	ld	t1, 0(t0)
	sd	t1, CKPT_RETURN_ADDRESS(s0)
	LOAD_ADDR t0, instruction_cnt
	ld	t1, 0(t0)
	sd	t1, CKPT_INSTRUCTION_CNT(s0)
	LOAD_ADDR t0, memory_history_top
	ld	t1, 0(t0)
	sd	t1, CKPT_MEMORY_HISTORY_TOP(s0)
	LOAD_ADDR t0, guard_list_top
	ld	t1, 0(t0)
	sd	t1, CKPT_GUARD_LIST_TOP(s0)

#ifdef COVERAGE
	LOAD_ADDR t0, checkpoint_cnt
	ld	t0, 0(t0)
	li	t1, 1
	bgt	t0, t1, .Lno_normal_coverage
	ld	a0, CKPT_RETURN_ADDRESS(s0)
	call	hfuzz_trace_pc
.Lno_normal_coverage:
#endif

#ifdef TIME
	RDCYCLE_STAT t1, t3, t4
	LOAD_ADDR_OFFSET t3, simulation_statistics, 16
	ld	t4, 0(t3)
	add	t4, t4, t1
	sd	t4, 0(t3)
#endif

	RESTORE_FROM_CHECKPOINT_TARGET s0, CHECKPOINT_TARGET_TRAMPOLINE_ADDR

.Lskip_checkpoint:
	RESTORE_FROM_TEMP CHECKPOINT_TARGET_RETURN_ADDR
.Lmake_checkpoint_end:
	.size	make_checkpoint_riscv64, .Lmake_checkpoint_end-make_checkpoint_riscv64
	.cfi_endproc

	.globl	restore_checkpoint_registers
	.p2align	2
	.type	restore_checkpoint_registers,@function
restore_checkpoint_registers:
	.cfi_startproc
	LOAD_ADDR t0, checkpoint_cnt
	ld	t2, 0(t0)
	LOAD_ADDR t0, checkpoint_metadata
	slli	t1, t2, CHECKPOINT_METADATA_SHIFT
	add	t0, t0, t1

	LOAD_ADDR t1, processor_extended_states
	slli	t2, t2, PROCESSOR_EXTENDED_STATE_SHIFT
	add	t1, t1, t2
	RESTORE_FLOAT_STATE t1

#ifdef TIME
	mv	s0, t0
	RDCYCLE_STAT t1, t3, t4
	LOAD_ADDR_OFFSET t3, simulation_statistics, 24
	ld	t4, 0(t3)
	add	t4, t4, t1
	sd	t4, 0(t3)
	mv	t0, s0
#endif

	ld	t1, CKPT_RETURN_ADDRESS(t0)
	LOAD_ADDR_OFFSET t2, checkpoint_target_metadata, CHECKPOINT_TARGET_RETURN_ADDR
	sd	t1, 0(t2)
	RESTORE_FROM_CHECKPOINT t0
.Lrestore_checkpoint_registers_end:
	.size	restore_checkpoint_registers, .Lrestore_checkpoint_registers_end-restore_checkpoint_registers
	.cfi_endproc

	.bss
	.globl	checkpoint_target_metadata
	.p2align	3
	.type	checkpoint_target_metadata,@object
checkpoint_target_metadata:
	.zero	CHECKPOINT_TARGET_METADATA_SIZE
	.size	checkpoint_target_metadata, CHECKPOINT_TARGET_METADATA_SIZE

	.globl	max_checkpoints
	.p2align	3
	.type	max_checkpoints,@object
max_checkpoints:
	.zero	8
	.size	max_checkpoints, 8

	.section	.note.GNU-stack,"",@progbits
