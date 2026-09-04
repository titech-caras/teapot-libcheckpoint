	.text
	.globl checkpoint_x64_simd_normal_path_probe
	.type checkpoint_x64_simd_normal_path_probe,@function
checkpoint_x64_simd_normal_path_probe:
	leaq .Lnormal_path(%rip), %rax
	movq %rax, checkpoint_target_metadata(%rip)
	leaq .Lrollback_path(%rip), %rax
	movq %rax, checkpoint_target_metadata+8(%rip)
	movabsq $0x0123456789abcdef, %rax
	movq %rax, %xmm0
	jmp make_checkpoint_x64

.Lnormal_path:
	movq %xmm0, %rax
	movabsq $0x0123456789abcdef, %rdx
	cmpq %rdx, %rax
	sete %al
	movzbl %al, %eax
	movq $0, checkpoint_cnt(%rip)
	retq

.Lrollback_path:
	xorl %eax, %eax
	movq $0, checkpoint_cnt(%rip)
	retq

.Lcheckpoint_x64_simd_normal_path_probe_end:
	.size checkpoint_x64_simd_normal_path_probe, .Lcheckpoint_x64_simd_normal_path_probe_end-checkpoint_x64_simd_normal_path_probe

	.section .note.GNU-stack,"",@progbits
