/* In MAKE_CHECKPOINT: rax/rbx are already saved. Keep the other temporaries
 * on the runtime stack, including on a refused checkpoint. No application
 * store, instrumentation, or C call occurs in this decision. */
	movq checkpoint_cnt, %rax
	cmpq $MAX_CHECKPOINTS, %rax
	jge .Lskip_checkpoint\@
	pushq %rcx
	pushq %rdx
	pushq %rsi
	movq checkpoint_target_metadata+CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR, %rbx
	movzbl checkpoint_target_metadata+CHECKPOINT_TARGET_OUTCOME, %ecx
	leaq (%rbx,%rcx,2), %rsi
	testq %rax, %rax
	jnz .Lhashed_tree\@
	movzwl (%rsi), %edx
	cmpl $0xffff, %edx
	je .Lhashed_skip\@
	incl %edx
	movw %dx, (%rsi)
	leal -1(%rdx), %ecx
	testl %edx, %ecx
	jnz .Lhashed_skip\@
	movq $MAX_CHECKPOINTS, max_checkpoints
	incl teapot_nested_root_gen
	jnz .Lhashed_allow\@
	incl teapot_nested_root_gen
	jmp .Lhashed_allow\@
.Lhashed_tree\@:
	movl teapot_nested_root_gen, %eax
	cmpl %eax, 4(%rbx)
	je .Lhashed_generation_ready\@
	movl %eax, 4(%rbx)
	movl $0, 8(%rbx)
.Lhashed_generation_ready\@:
	movzwl 8(%rsi), %edx
	cmpl $0xffff, %edx
	je .Lhashed_skip\@
	incl %edx
	movw %dx, 8(%rsi)
	cmpl $0xffff, %edx
	je .Lhashed_skip\@
	/* key = (u64(root_gen) << 24) ^ (site_index << 1) ^ outcome */
	shlq $24, %rax
	movq checkpoint_target_metadata+CHECKPOINT_TARGET_SITE_INDEX, %rsi
	shlq $1, %rsi
	xorq %rax, %rsi
	xorq %rcx, %rsi
	xorq teapot_nested_seed_word, %rsi
	movabsq $0xbf58476d1ce4e5b9, %rbx
	imulq %rbx, %rsi
	movq %rsi, %rax
	shrq $31, %rax
	xorq %rax, %rsi
	bsrl %edx, %ecx
	movabsq $0x94d049bb133111eb, %rax
	imulq %rcx, %rax
	addq %rax, %rsi
	movq %rsi, %rax
	shrq $29, %rax
	xorq %rax, %rsi
	imulq %rbx, %rsi
	movq %rsi, %rax
	shrq $32, %rax
	xorq %rax, %rsi
	movl $1, %eax
	shll %cl, %eax
	subl %eax, %edx
	decl %eax
	andl %esi, %eax
	cmpl %edx, %eax
	jne .Lhashed_skip\@
.Lhashed_allow\@:
	popq %rsi
	popq %rdx
	popq %rcx
	movq checkpoint_cnt, %rax
	jmp .Lhashed_done\@
.Lhashed_skip\@:
	popq %rsi
	popq %rdx
	popq %rcx
	jmp .Lskip_checkpoint\@
.Lhashed_done\@:
