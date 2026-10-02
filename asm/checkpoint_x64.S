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

.macro SAVE_BOOTSTRAP_REGS df=0
	pushq	%rax
	lahf
	seto	%al
	movzwl	%ax, %eax
	pushq	%rax
	pushq	%rbx
	.if \df
	/* The uncommon entry additionally records DF in bit 16 of the packed
	 * arithmetic flags. Only this path reads full flags, on the private stack. */
	pushfq
	popq	%rax
	andl	$0x400, %eax
	shll	$6, %eax
	orq	%rax, 8(%rsp)
	.endif
	cld
.endm

.macro RESTORE_BOOTSTRAP_REGS_AND_JUMP target
	popq	%rbx
	popq	%rax
	RESTORE_PACKED_FLAGS
	popq	%rax
	movq	old_rsp, %rsp
	jmpq	*\target
.endm

.macro RESTORE_PACKED_FLAGS
	testl	$0x10000, %eax
	jz	.Ldf_clear\@
	std
.Ldf_clear\@:
	addb	$0x7f, %al
	sahf
.endm

.macro SAVE_SIMD_STATE base, checkpoint=0
	.if \checkpoint
	movq	checkpoint_xsave_mask, %rax
	.else
	movq	processor_xsave_mask, %rax
	.endif
	testq	%rax, %rax
	jz	.Lsave_fx\@
	movq	%rax, %rdx
	shrq	$32, %rdx
	cmpq	$0, processor_has_xsaveopt
	je	.Lsave_xsave\@
	xsaveopt64	(\base)
	jmp	.Lsave_simd_done\@
.Lsave_xsave\@:
	xsave64	(\base)
	jmp	.Lsave_simd_done\@
.Lsave_fx\@:
	fxsave64	(\base)
.Lsave_simd_done\@:
.endm

.macro SAVE_LOW_XMM base
	movaps	%xmm0, PROCESSOR_XMM_STATE_OFFSET(\base)
	movaps	%xmm1, PROCESSOR_XMM_STATE_OFFSET+16(\base)
	movaps	%xmm2, PROCESSOR_XMM_STATE_OFFSET+32(\base)
	movaps	%xmm3, PROCESSOR_XMM_STATE_OFFSET+48(\base)
	movaps	%xmm4, PROCESSOR_XMM_STATE_OFFSET+64(\base)
	movaps	%xmm5, PROCESSOR_XMM_STATE_OFFSET+80(\base)
	movaps	%xmm6, PROCESSOR_XMM_STATE_OFFSET+96(\base)
	movaps	%xmm7, PROCESSOR_XMM_STATE_OFFSET+112(\base)
.endm

.macro RESTORE_SIMD_STATE base, checkpoint=0
	.if \checkpoint
	movq	checkpoint_xsave_mask, %rax
	.else
	movq	processor_xsave_mask, %rax
	.endif
	testq	%rax, %rax
	jz	.Lrestore_fx\@
	movq	%rax, %rdx
	shrq	$32, %rdx
	xrstor64	(\base)
	jmp	.Lrestore_simd_done\@
.Lrestore_fx\@:
	fxrstor64	(\base)
.Lrestore_simd_done\@:
.endm

.macro RESTORE_LOW_XMM base
	movaps	PROCESSOR_XMM_STATE_OFFSET(\base), %xmm0
	movaps	PROCESSOR_XMM_STATE_OFFSET+16(\base), %xmm1
	movaps	PROCESSOR_XMM_STATE_OFFSET+32(\base), %xmm2
	movaps	PROCESSOR_XMM_STATE_OFFSET+48(\base), %xmm3
	movaps	PROCESSOR_XMM_STATE_OFFSET+64(\base), %xmm4
	movaps	PROCESSOR_XMM_STATE_OFFSET+80(\base), %xmm5
	movaps	PROCESSOR_XMM_STATE_OFFSET+96(\base), %xmm6
	movaps	PROCESSOR_XMM_STATE_OFFSET+112(\base), %xmm7
.endm

/* These are assembler-time choices, not register-mask tests at run time.
 * The full path retains the existing CPU-feature fallback for older hosts. */
.macro SAVE_CHECKPOINT_VECTORS base, variant
	.if \variant == 1
	SAVE_LOW_XMM \base
	.elseif \variant == 2
	SAVE_SIMD_STATE \base, 1
	.endif
.endm

.macro RESTORE_CHECKPOINT_VECTORS base, variant
	.if \variant == 1
	RESTORE_LOW_XMM \base
	.elseif \variant == 2
	RESTORE_SIMD_STATE \base, 1
	.endif
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
	/* Packed arithmetic flags and DF need no application-stack writes. */
	movq	CKPT_FLAGS(%rbx), %rax
	RESTORE_PACKED_FLAGS
	movq	CKPT_RSP(%rbx), %rsp
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
	cld	/* Every rollback reason enters C with the ABI direction flag. */
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
	pushfq
	.cfi_def_cfa_offset 16
	pushq	%rbx
	.cfi_def_cfa_offset 24
	.cfi_offset %rbx, -24
	cld
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
	.cfi_def_cfa_offset 16
	.cfi_restore %rbx
	popfq
	.cfi_def_cfa_offset 8
	retq
.Lreport_gadget_\type\()_end:
	.size	report_gadget_\type, .Lreport_gadget_\type\()_end-report_gadget_\type
	.cfi_endproc
.endm

DEF_REPORT_GADGET_X64_PRESERVE KASPER_CACHE
DEF_REPORT_GADGET_X64_PRESERVE KASPER_MDS
DEF_REPORT_GADGET_X64_PRESERVE KASPER_PORT

.macro DEF_MAKE_CHECKPOINT name, variant
	.globl	\name
	.globl	\name\()_df
	.p2align	4, 0x90
	.type	\name\()_df,@function
\name\()_df:
	.cfi_startproc
	X64_SWITCH_TO_SCRATCHPAD_STACK
	SAVE_BOOTSTRAP_REGS 1
	jmp	.Lcheckpoint_common\@
	.size	\name\()_df, .-\name\()_df
	.cfi_endproc

	.p2align	4, 0x90
	.type	\name,@function
\name:
	.cfi_startproc
	X64_SWITCH_TO_SCRATCHPAD_STACK
	SAVE_BOOTSTRAP_REGS
.Lcheckpoint_common\@:
#if TEAPOT_X64_VECTOR_MODE == 1
	.set .Lvector_variant\@, 1
#elif TEAPOT_X64_VECTOR_MODE > 1
	.set .Lvector_variant\@, 2
#else
	.set .Lvector_variant\@, \variant
#endif
	cmpb	$0, libcheckpoint_enabled
	je	.Lskip_checkpoint\@

	movq	checkpoint_cnt, %rax
	testq	%rax, %rax
	jnz	.Lif_should_checkpoint\@

#ifdef USE_BRANCH_EXEC_COUNT
	movq	checkpoint_target_metadata+16, %rbx
	incl	(%rbx)
	movl	(%rbx), %ebx
#endif

#ifdef BRANCH_FULL_EXEC_COUNT
	cmpq	$BRANCH_FULL_EXEC_COUNT, %rbx
	jg	.Lheuristic\@
	movq	$MAX_CHECKPOINTS, max_checkpoints
	jmp	.Lif_should_checkpoint\@
#endif

.Lheuristic\@:
#ifdef SPECFUZZ_PRIORITIZED_SIMULATION
	tzcntq	%rbx, %rbx
	shrq	$1, %rbx
	incq	%rbx
	cmpq	$MAX_CHECKPOINTS, %rbx
	jbe	.Ldepth_limit_ready\@
	movq	$MAX_CHECKPOINTS, %rbx
.Ldepth_limit_ready\@:
	movq	%rbx, max_checkpoints
#else
	movq	$1, max_checkpoints
#endif

.Lif_should_checkpoint\@:
	cmpq	max_checkpoints, %rax
	jge	.Lskip_checkpoint\@
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

	SAVE_CHECKPOINT_VECTORS %rbx, .Lvector_variant\@

	popq	%rdx
	popq	%rax

	leaq	checkpoint_metadata, %rbx
	shlq	$CHECKPOINT_METADATA_SHIFT, %rax
	addq	%rbx, %rax
	STORE_REGS_TO_CHECKPOINT %rax
	COPY_DIFT_TAGS %rax
	STORE_RUNTIME_STATE_TO_CHECKPOINT %rax
	.if .Lvector_variant\@ == 0
	leaq	.Lrestore_vectors_integer(%rip), %rbx
	.elseif .Lvector_variant\@ == 1
	leaq	.Lrestore_vectors_xmm(%rip), %rbx
	.else
	leaq	.Lrestore_vectors_full(%rip), %rbx
	.endif
	movq	%rbx, CKPT_VECTOR_RESTORE(%rax)
	/* Even a dead vector calculation can change floating-point exception
	 * status. Preserve MXCSR independently of vector-register liveness. */
	stmxcsr	CKPT_MXCSR(%rax)

#ifdef COVERAGE
	cmpq	$1, checkpoint_cnt
	jg	.Lno_normal_coverage\@
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
	RESTORE_CHECKPOINT_VECTORS %r10, .Lvector_variant\@
	ldmxcsr	CKPT_MXCSR(%rbx)
	movq	%rbx, %rax
	RESTORE_COVERAGE_CLOBBERS_FROM_CHECKPOINT %rax
.Lno_normal_coverage\@:
#endif

#ifdef TIME
	movq	%rax, %rbx
	RDTSC_STAT
	addq	%rax, simulation_statistics+16
	movq	CKPT_RDX(%rbx), %rdx
#endif

	RESTORE_BOOTSTRAP_REGS_AND_JUMP checkpoint_target_metadata

.Lskip_checkpoint\@:
	RESTORE_BOOTSTRAP_REGS_AND_JUMP checkpoint_target_metadata+8
	.size	\name, .-\name
	.cfi_endproc
.endm

DEF_MAKE_CHECKPOINT make_checkpoint_integer, 0
DEF_MAKE_CHECKPOINT make_checkpoint_xmm, 1
DEF_MAKE_CHECKPOINT make_checkpoint_x64, 2

/* Preserve the public full/DF entry used by existing instrumented binaries. */
	.globl make_checkpoint_df
	.set make_checkpoint_df, make_checkpoint_x64_df

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
	/* Each depth owns its restore recipe, independent of the latest site. */
	jmpq	*CKPT_VECTOR_RESTORE(%r11)

.Lrestore_vectors_full:
	RESTORE_SIMD_STATE %r8, 1
	jmp	.Lrestore_vectors_integer
.Lrestore_vectors_xmm:
	RESTORE_LOW_XMM %r8
.Lrestore_vectors_integer:
	movq	%r11, %rax
	ldmxcsr	CKPT_MXCSR(%rax)

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

	.section	.note.GNU-stack,"",@progbits
