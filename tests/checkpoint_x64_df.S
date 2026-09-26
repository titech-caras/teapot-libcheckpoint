#include "checkpoint.h"

	.text
	.globl checkpoint_df_probe
	.type checkpoint_df_probe,@function
checkpoint_df_probe:
	pushq %rbx
	leaq .Ldf_transient(%rip), %rax
	movq %rax, checkpoint_target_metadata+CHECKPOINT_TARGET_TRAMPOLINE_ADDR(%rip)
	leaq .Ldf_restored(%rip), %rax
	movq %rax, checkpoint_target_metadata+CHECKPOINT_TARGET_RETURN_ADDR(%rip)
	movq $0x12345678, %rbx
	movq $0x23456789, %rcx
	movq $0x34567890, %rdx
	pushfq
	popq %rax
	andq $~0xcd5, %rax
	orq %rdi, %rax
	pushq %rax
	popfq
	movq $0x45678901, -16(%rsp)
	jmp make_checkpoint_x64
.Ldf_transient:
	pushfq
	popq checkpoint_df_seen+8(%rip)
	/* Force rollback with the opposite DF; only the checkpoint is authoritative. */
	movq checkpoint_df_seen+8(%rip), %rax
	xorq $0x400, %rax
	pushq %rax
	popfq
	jmp restore_checkpoint_ROB_LEN
.Ldf_restored:
	pushfq
	popq checkpoint_df_seen(%rip)
	cld
	movq %rbx, checkpoint_df_seen+16(%rip)
	movq %rcx, checkpoint_df_seen+24(%rip)
	movq %rdx, checkpoint_df_seen+32(%rip)
	movq -16(%rsp), %rax
	movq %rax, checkpoint_df_seen+40(%rip)
	popq %rbx
	ret
	.size checkpoint_df_probe, .-checkpoint_df_probe
	.section .note.GNU-stack,"",@progbits
