#include "checkpoint.h"

.macro RDTSC_STAT
	rdtsc
	shlq	$32, %rdx
	orq	%rdx, %rax
	movq	%rax, %rdx
	subq	last_rdtsc, %rax
	movq	%rdx, last_rdtsc
.endm

.macro X64_SWITCH_TO_SCRATCHPAD_STACK
	movq	%rsp, old_rsp
	leaq	scratchpad+SCRATCHPAD_SIZE-8, %rsp
.endm

.macro SAVE_BOOTSTRAP_REGS
	pushq	%rax
	lahf
	seto	%al
	pushq	%rax
	pushq	%rbx
.endm

.macro RESTORE_BOOTSTRAP_REGS_AND_JUMP target
	popq	%rbx
	popq	%rax
	addb	$0x7f, %al
	sahf
	popq	%rax
	movq	old_rsp, %rsp
	jmpq	*\target
.endm

.macro SAVE_SIMD_STATE base
	movq	processor_xsave_mask, %rax
	testq	%rax, %rax
	jz	.Lsave_xmm_only\@
	movq	%rax, %rdx
	shrq	$32, %rdx
	xsave64	(\base)
	jmp	.Lsave_simd_done\@
.Lsave_xmm_only\@:
	movl	$0xFFFFFFFF, %eax
	movl	$0xFFFFFFFF, %edx
	movaps	%xmm0, (\base)
	movaps	%xmm1, 16(\base)
	movaps	%xmm2, 32(\base)
	movaps	%xmm3, 48(\base)
	movaps	%xmm4, 64(\base)
	movaps	%xmm5, 80(\base)
	movaps	%xmm6, 96(\base)
	movaps	%xmm7, 112(\base)
	movaps	%xmm8, 128(\base)
	movaps	%xmm9, 144(\base)
	movaps	%xmm10, 160(\base)
	movaps	%xmm11, 176(\base)
	movaps	%xmm12, 192(\base)
	movaps	%xmm13, 208(\base)
	movaps	%xmm14, 224(\base)
	movaps	%xmm15, 240(\base)
.Lsave_simd_done\@:
.endm

.macro RESTORE_SIMD_STATE base
	movq	processor_xsave_mask, %rax
	testq	%rax, %rax
	jz	.Lrestore_xmm_only\@
	movq	%rax, %rdx
	shrq	$32, %rdx
	xrstor64	(\base)
	jmp	.Lrestore_simd_done\@
.Lrestore_xmm_only\@:
	movl	$0xFFFFFFFF, %eax
	movl	$0xFFFFFFFF, %edx
	movaps	(\base), %xmm0
	movaps	16(\base), %xmm1
	movaps	32(\base), %xmm2
	movaps	48(\base), %xmm3
	movaps	64(\base), %xmm4
	movaps	80(\base), %xmm5
	movaps	96(\base), %xmm6
	movaps	112(\base), %xmm7
	movaps	128(\base), %xmm8
	movaps	144(\base), %xmm9
	movaps	160(\base), %xmm10
	movaps	176(\base), %xmm11
	movaps	192(\base), %xmm12
	movaps	208(\base), %xmm13
	movaps	224(\base), %xmm14
	movaps	240(\base), %xmm15
.Lrestore_simd_done\@:
.endm

/*
 * Gadget reports are ordinary C calls and may overwrite every caller-saved
 * SIMD register.  Report sites can be inserted between an assembly load and a
 * later use of the loaded vector, so preserve the complete x86-64 XMM register
 * file around the call.  The XSAVE image has dedicated 64-byte-aligned storage
 * whose reserved header bytes remain zero; the scratchpad is still used only
 * for the report call stack and the tag spill.
 */
#define X64_REPORT_CALL_STACK_OFFSET (SCRATCHPAD_SIZE - 32768)
#define X64_REPORT_TAG_OFFSET (SCRATCHPAD_SIZE - 64)

.macro COPY_DIFT_TAGS ckpt
#ifdef ENABLE_DIFT
	movaps	%xmm0, scratchpad
	movaps	dift_reg_tags, %xmm0
	movaps	%xmm0, CKPT_DIFT_REG_TAGS(\ckpt)
	movaps	dift_reg_tags+16, %xmm0
	movaps	%xmm0, CKPT_DIFT_REG_TAGS+16(\ckpt)
	movaps	dift_reg_tags+32, %xmm0
	movaps	%xmm0, CKPT_DIFT_REG_TAGS+32(\ckpt)
	movaps	scratchpad, %xmm0
#endif
.endm

.macro STORE_REGS_TO_CHECKPOINT ckpt
	movq	(%rsp), %rbx
	movq	%rbx, CKPT_RBX(\ckpt)
	movq	16(%rsp), %rbx
	movq	%rbx, CKPT_RAX(\ckpt)
	movq	8(%rsp), %rbx
	movq	%rbx, CKPT_FLAGS(\ckpt)
	movq	old_rsp, %rbx
	movq	%rbx, CKPT_RSP(\ckpt)
	movq	%rcx, CKPT_RCX(\ckpt)
	movq	%rdx, CKPT_RDX(\ckpt)
	movq	%rsi, CKPT_RSI(\ckpt)
	movq	%rdi, CKPT_RDI(\ckpt)
	movq	%rbp, CKPT_RBP(\ckpt)
	movq	%r8, CKPT_R8(\ckpt)
	movq	%r9, CKPT_R9(\ckpt)
	movq	%r10, CKPT_R10(\ckpt)
	movq	%r11, CKPT_R11(\ckpt)
	movq	%r12, CKPT_R12(\ckpt)
	movq	%r13, CKPT_R13(\ckpt)
	movq	%r14, CKPT_R14(\ckpt)
	movq	%r15, CKPT_R15(\ckpt)
.endm

.macro STORE_RUNTIME_STATE_TO_CHECKPOINT ckpt
	movq	checkpoint_target_metadata+8, %rbx
	movq	%rbx, CKPT_RETURN_ADDRESS(\ckpt)
	movq	instruction_cnt, %rbx
	movq	%rbx, CKPT_INSTRUCTION_CNT(\ckpt)
	movq	memory_history_top, %rbx
	movq	%rbx, CKPT_MEMORY_HISTORY_TOP(\ckpt)
	movq	guard_list_top, %rbx
	movq	%rbx, CKPT_GUARD_LIST_TOP(\ckpt)
.endm

.macro RESTORE_COVERAGE_CLOBBERS_FROM_CHECKPOINT ckpt
	movq	CKPT_RCX(\ckpt), %rcx
	movq	CKPT_RDX(\ckpt), %rdx
	movq	CKPT_RSI(\ckpt), %rsi
	movq	CKPT_RDI(\ckpt), %rdi
	movq	CKPT_R8(\ckpt), %r8
	movq	CKPT_R9(\ckpt), %r9
	movq	CKPT_R10(\ckpt), %r10
	movq	CKPT_R11(\ckpt), %r11
.endm

.macro RESTORE_GPRS_FROM_CHECKPOINT ckpt
	movq	CKPT_RCX(\ckpt), %rcx
	movq	CKPT_RDX(\ckpt), %rdx
	movq	CKPT_RSI(\ckpt), %rsi
	movq	CKPT_RDI(\ckpt), %rdi
	movq	CKPT_RSP(\ckpt), %rsp
	movq	CKPT_RBP(\ckpt), %rbp
	movq	CKPT_R8(\ckpt), %r8
	movq	CKPT_R9(\ckpt), %r9
	movq	CKPT_R10(\ckpt), %r10
	movq	CKPT_R11(\ckpt), %r11
	movq	CKPT_R12(\ckpt), %r12
	movq	CKPT_R13(\ckpt), %r13
	movq	CKPT_R14(\ckpt), %r14
	movq	CKPT_R15(\ckpt), %r15
.endm

.macro RESTORE_FLAGS_RAX_RBX_AND_JUMP ckpt
	movq	\ckpt, %rbx
	movq	CKPT_FLAGS(%rbx), %rax
	addb	$0x7f, %al
	sahf
	movq	CKPT_RETURN_ADDRESS(%rbx), %rax
	movq	%rax, checkpoint_target_metadata+8
	movq	CKPT_RAX(%rbx), %rax
	movq	CKPT_RBX(%rbx), %rbx
	jmpq	*checkpoint_target_metadata+8
.endm

.macro DEF_RESTORE_CHECKPOINT name, reason
	.globl	restore_checkpoint_\name
	.p2align	4, 0x90
	.type	restore_checkpoint_\name,@function
restore_checkpoint_\name:
	.cfi_startproc
	X64_SWITCH_TO_SCRATCHPAD_STACK
	movq	$\reason, %rdi
	jmp	restore_checkpoint
.Lrestore_checkpoint_\name\()_end:
	.size	restore_checkpoint_\name, .Lrestore_checkpoint_\name\()_end-restore_checkpoint_\name
	.cfi_endproc
.endm

	.text
DEF_RESTORE_CHECKPOINT ROB_LEN, ROLLBACK_ROB_LEN
DEF_RESTORE_CHECKPOINT SIGSEGV, ROLLBACK_SIGSEGV
DEF_RESTORE_CHECKPOINT EXT_LIB, ROLLBACK_EXT_LIB
DEF_RESTORE_CHECKPOINT MALFORMED_INDIRECT_BR, ROLLBACK_MALFORMED_INDIRECT_BR

.macro DEF_REPORT_GADGET_X64_PRESERVE type
	.globl	report_gadget_\type
	.p2align	4, 0x90
	.type	report_gadget_\type,@function
report_gadget_\type:
	.cfi_startproc
	pushq	%rbx
	.cfi_def_cfa_offset 16
	.cfi_offset %rbx, -16
	movq	%rsp, %rbx
	.cfi_def_cfa_register %rbx
	movq	%rdx, scratchpad+X64_REPORT_TAG_OFFSET
	leaq	report_extended_state(%rip), %r10
	SAVE_SIMD_STATE %r10
	movq	scratchpad+X64_REPORT_TAG_OFFSET, %rdx
	leaq	scratchpad+X64_REPORT_CALL_STACK_OFFSET, %rsp
	callq	report_gadget_x64_impl_\type
	leaq	report_extended_state(%rip), %r10
	RESTORE_SIMD_STATE %r10
	movq	%rbx, %rsp
	.cfi_def_cfa_register %rsp
	popq	%rbx
	.cfi_def_cfa_offset 8
	.cfi_restore %rbx
	retq
.Lreport_gadget_\type\()_end:
	.size	report_gadget_\type, .Lreport_gadget_\type\()_end-report_gadget_\type
	.cfi_endproc
.endm

DEF_REPORT_GADGET_X64_PRESERVE KASPER_CACHE
DEF_REPORT_GADGET_X64_PRESERVE KASPER_MDS
DEF_REPORT_GADGET_X64_PRESERVE KASPER_PORT

	.globl	make_checkpoint_x64
	.p2align	4, 0x90
	.type	make_checkpoint_x64,@function
make_checkpoint_x64:
	.cfi_startproc
	X64_SWITCH_TO_SCRATCHPAD_STACK
	SAVE_BOOTSTRAP_REGS

	cmpb	$0, libcheckpoint_enabled
	je	.Lskip_checkpoint

	movq	checkpoint_cnt, %rax
	testq	%rax, %rax
	jnz	.Lif_should_checkpoint

#ifdef USE_BRANCH_EXEC_COUNT
	movq	checkpoint_target_metadata+16, %rbx
	incl	(%rbx)
	movl	(%rbx), %ebx
#endif

#ifdef BRANCH_FULL_EXEC_COUNT
	cmpq	$BRANCH_FULL_EXEC_COUNT, %rbx
	jg	.Lheuristic
	movq	$MAX_CHECKPOINTS, max_checkpoints
	jmp	.Lif_should_checkpoint
#endif

.Lheuristic:
#ifdef SPECFUZZ_PRIORITIZED_SIMULATION
	tzcntq	%rbx, %rbx
	shrq	$1, %rbx
	incq	%rbx
	movq	%rbx, max_checkpoints
#else
	movq	$1, max_checkpoints
#endif

.Lif_should_checkpoint:
	cmpq	max_checkpoints, %rax
	jge	.Lskip_checkpoint
	incl	checkpoint_cnt

	pushq	%rax
	pushq	%rdx
	leaq	processor_extended_states, %rbx
	shlq	$PROCESSOR_EXTENDED_STATE_SHIFT, %rax
	addq	%rax, %rbx

#ifdef TIME
	RDTSC_STAT
	xorq	%rdx, %rdx
	cmpq	$1, checkpoint_cnt
	setg	%dl
	addq	%rax, simulation_statistics(,%rdx,8)
#endif

	SAVE_SIMD_STATE %rbx

	popq	%rdx
	popq	%rax

	leaq	checkpoint_metadata, %rbx
	shlq	$CHECKPOINT_METADATA_SHIFT, %rax
	addq	%rbx, %rax
	STORE_REGS_TO_CHECKPOINT %rax
	COPY_DIFT_TAGS %rax
	STORE_RUNTIME_STATE_TO_CHECKPOINT %rax

#ifdef COVERAGE
	cmpq	$1, checkpoint_cnt
	jg	.Lno_normal_coverage
	movq	checkpoint_target_metadata+8, %rdi
	movq	%rax, %rbx
	callq	hfuzz_trace_pc
	/*
	 * The coverage hook is an ordinary ABI call and may clobber every
	 * caller-saved SIMD register.  The snapshot above is also the state that
	 * must remain live on the normal checkpoint path, not just on rollback.
	 * Recompute its slot before restoring the saved GPRs; those restores undo
	 * the scratch-register clobbers made here.
	 */
	leaq	processor_extended_states, %r10
	movq	checkpoint_cnt, %r11
	decq	%r11
	shlq	$PROCESSOR_EXTENDED_STATE_SHIFT, %r11
	addq	%r11, %r10
	RESTORE_SIMD_STATE %r10
	movq	%rbx, %rax
	RESTORE_COVERAGE_CLOBBERS_FROM_CHECKPOINT %rax
.Lno_normal_coverage:
#endif

#ifdef TIME
	movq	%rax, %rbx
	RDTSC_STAT
	addq	%rax, simulation_statistics+16
	movq	CKPT_RDX(%rbx), %rdx
#endif

	RESTORE_BOOTSTRAP_REGS_AND_JUMP checkpoint_target_metadata

.Lskip_checkpoint:
	RESTORE_BOOTSTRAP_REGS_AND_JUMP checkpoint_target_metadata+8
.Lmake_checkpoint_end:
	.size	make_checkpoint_x64, .Lmake_checkpoint_end-make_checkpoint_x64
	.cfi_endproc

	.globl	restore_checkpoint_registers
	.p2align	4, 0x90
	.type	restore_checkpoint_registers,@function
restore_checkpoint_registers:
	.cfi_startproc
	movq	checkpoint_cnt, %rax
	movq	%rax, %r8
	leaq	checkpoint_metadata, %rbx
	shlq	$CHECKPOINT_METADATA_SHIFT, %rax
	addq	%rbx, %rax

	movq	%rax, %r11
	leaq	processor_extended_states, %r9
	shlq	$PROCESSOR_EXTENDED_STATE_SHIFT, %r8
	addq	%r9, %r8
	RESTORE_SIMD_STATE %r8
	movq	%r11, %rax

	RESTORE_GPRS_FROM_CHECKPOINT %rax

#ifdef TIME
	movq	%rax, %rbx
	RDTSC_STAT
	addq	%rax, simulation_statistics+24
	movq	%rbx, %rax
	movq	CKPT_RDX(%rax), %rdx
#endif

	RESTORE_FLAGS_RAX_RBX_AND_JUMP %rax
.Lrestore_checkpoint_registers_end:
	.size	restore_checkpoint_registers, .Lrestore_checkpoint_registers_end-restore_checkpoint_registers
	.cfi_endproc

	.bss
	.globl	checkpoint_target_metadata
	.p2align	3, 0x0
	.type	checkpoint_target_metadata,@object
checkpoint_target_metadata:
	.zero	CHECKPOINT_TARGET_METADATA_SIZE
	.size	checkpoint_target_metadata, CHECKPOINT_TARGET_METADATA_SIZE

	.globl	max_checkpoints
	.p2align	3, 0x0
	.type	max_checkpoints,@object
max_checkpoints:
	.zero	8
	.size	max_checkpoints, 8

	.section	.note.GNU-stack,"",@progbits
