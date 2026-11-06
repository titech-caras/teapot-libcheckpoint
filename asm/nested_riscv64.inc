	LOAD_ADDR t0, checkpoint_cnt
	ld t2, 0(t0)
	li t1, MAX_CHECKPOINTS
	bge t2, t1, .Lskip_checkpoint
	LOAD_ADDR t0, checkpoint_target_metadata
	ld t1, CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR(t0)
	lbu t3, CHECKPOINT_TARGET_OUTCOME(t0)
	slli t4, t3, 1
	add t4, t1, t4
	li t6, 0xffff
	bnez t2, .Lhashed_tree
	lhu t5, 0(t4)
	beq t5, t6, .Lskip_checkpoint
	addi t5, t5, 1
	sh t5, 0(t4)
	addi t6, t5, -1
	and t6, t6, t5
	bnez t6, .Lskip_checkpoint
	LOAD_ADDR t0, teapot_nested_root_gen
	lw t1, 0(t0)
	addiw t1, t1, 1
	bnez t1, .Lhashed_root_ready
	li t1, 1
.Lhashed_root_ready:
	sw t1, 0(t0)
	j .Lhashed_done
.Lhashed_tree:
	LOAD_ADDR s0, teapot_nested_root_gen
	lwu s0, 0(s0)
	lwu t0, 4(t1)
	beq s0, t0, .Lhashed_generation_ready
	sw s0, 4(t1)
	sw zero, 8(t1)
.Lhashed_generation_ready:
	lhu t5, 8(t4)
	beq t5, t6, .Lskip_checkpoint
	addi t5, t5, 1
	sh t5, 8(t4)
	li t0, 1
	beq t5, t0, .Lhashed_done
	beq t5, t6, .Lskip_checkpoint
	slli s0, s0, 24
	LOAD_ADDR t0, checkpoint_target_metadata
	ld t1, CHECKPOINT_TARGET_SITE_INDEX(t0)
	slli t1, t1, 1
	xor s0, s0, t1
	xor s0, s0, t3
	LOAD_ADDR t1, teapot_nested_seed_word
	ld t1, 0(t1)
	xor s0, s0, t1
	li t6, 0xbf58476d1ce4e5b9
	mul s0, s0, t6
	srli t1, s0, 31
	xor s0, s0, t1
	/* RV64 baseline has no CLZ. At most fifteen shifts for a u16 count. */
	li t3, 0
	mv t4, t5
.Lhashed_octave:
	srli t4, t4, 1
	beqz t4, .Lhashed_octave_ready
	addi t3, t3, 1
	j .Lhashed_octave
.Lhashed_octave_ready:
	li t1, 0x94d049bb133111eb
	mul t1, t1, t3
	add s0, s0, t1
	srli t1, s0, 29
	xor s0, s0, t1
	mul s0, s0, t6
	srli t1, s0, 32
	xor s0, s0, t1
	li t4, 1
	sll t4, t4, t3
	sub t5, t5, t4
	addi t4, t4, -1
	and s0, s0, t4
	bne s0, t5, .Lskip_checkpoint
.Lhashed_done:
