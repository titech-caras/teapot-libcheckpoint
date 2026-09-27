	.text
.macro SELECT_PROFILE profile
	testl %edx, %edx
	jnz .Ldf_entry\@
	cmpl $0, \profile
	je make_checkpoint_integer
	cmpl $1, \profile
	je make_checkpoint_xmm
	jmp make_checkpoint_x64
.Ldf_entry\@:
	std
	cmpl $0, \profile
	je make_checkpoint_integer_df
	cmpl $1, \profile
	je make_checkpoint_xmm_df
	jmp make_checkpoint_df
.endm
	.globl checkpoint_x64_vector_cases_probe
	.type checkpoint_x64_vector_cases_probe,@function
checkpoint_x64_vector_cases_probe:
	pushq %rbx
	leaq .Louter_transient(%rip), %rax
	movq %rax, checkpoint_target_metadata(%rip)
	leaq .Louter_restored(%rip), %rax
	movq %rax, checkpoint_target_metadata+8(%rip)
	ldmxcsr .Lclean_mxcsr(%rip)
	movl $10, %eax
	movq %rax, %xmm0
	movl $15, %eax
	movq %rax, %xmm15
	SELECT_PROFILE %edi
.Louter_transient:
	testl %esi, %esi
	js .Labort_outer
	movq $2, max_checkpoints(%rip)
	leaq .Linner_transient(%rip), %rax
	movq %rax, checkpoint_target_metadata(%rip)
	leaq .Linner_restored(%rip), %rax
	movq %rax, checkpoint_target_metadata+8(%rip)
	movl $30, %eax
	movq %rax, %xmm0
	movl $35, %eax
	movq %rax, %xmm15
	SELECT_PROFILE %esi
.Linner_transient:
	pxor %xmm0, %xmm0
	pxor %xmm15, %xmm15
	ldmxcsr .Ldirty_mxcsr(%rip)
	jmp restore_checkpoint_ROB_LEN
.Linner_restored:
	pushfq
	popq vector_observed+56(%rip)
	movq %xmm0, vector_observed+24(%rip)
	movq %xmm15, vector_observed+32(%rip)
	stmxcsr vector_observed+40(%rip)
.Labort_outer:
	pxor %xmm0, %xmm0
	pxor %xmm15, %xmm15
	ldmxcsr .Ldirty_mxcsr(%rip)
	jmp restore_checkpoint_ROB_LEN
.Louter_restored:
	pushfq
	popq vector_observed+48(%rip)
	cld
	movq %xmm0, vector_observed(%rip)
	movq %xmm15, vector_observed+8(%rip)
	stmxcsr vector_observed+16(%rip)
	popq %rbx
	retq
	.size checkpoint_x64_vector_cases_probe, .-checkpoint_x64_vector_cases_probe
	.section .rodata
	.p2align 2
.Lclean_mxcsr: .long 0x1f80
.Ldirty_mxcsr: .long 0x3f80
	.section .note.GNU-stack,"",@progbits
