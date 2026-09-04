	.text
	.globl checkpoint_x64_xsave_report_probe
	.type checkpoint_x64_xsave_report_probe,@function
checkpoint_x64_xsave_report_probe:
	.cfi_startproc
	subq $8, %rsp
	.cfi_adjust_cfa_offset 8
	movabsq $0x0123456789abcdef, %rax
	movq %rax, %xmm0
	movabsq $0xfedcba9876543210, %rax
	movq %rax, %xmm15
	movabsq $0x1111222233334444, %rsi
	movl $0x55aa, %edx
	callq report_gadget_KASPER_CACHE
	movq %xmm0, %rax
	movabsq $0x0123456789abcdef, %rcx
	cmpq %rcx, %rax
	jne .Lxsave_report_mismatch
	movq %xmm15, %rax
	movabsq $0xfedcba9876543210, %rcx
	cmpq %rcx, %rax
	jne .Lxsave_report_mismatch
	movl $1, %eax
	jmp .Lxsave_report_done
.Lxsave_report_mismatch:
	xorl %eax, %eax
.Lxsave_report_done:
	addq $8, %rsp
	.cfi_adjust_cfa_offset -8
	retq
	.cfi_endproc
.Lcheckpoint_x64_xsave_report_probe_end:
	.size checkpoint_x64_xsave_report_probe, .Lcheckpoint_x64_xsave_report_probe_end-checkpoint_x64_xsave_report_probe

	.section .note.GNU-stack,"",@progbits
