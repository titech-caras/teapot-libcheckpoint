/* Only x0, x1, x16, x17 and x19: all saved by the existing bootstrap. */
	LOAD_ADDR x16, checkpoint_cnt
	ldr x0, [x16]
	cmp x0, #MAX_CHECKPOINTS
	b.ge .Lskip_checkpoint
	LOAD_ADDR x16, checkpoint_target_metadata
	ldr x17, [x16, #CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR]
	ldrb w1, [x16, #CHECKPOINT_TARGET_OUTCOME]
	add x19, x17, x1, lsl #1
	cbnz x0, .Lhashed_tree
	ldrh w0, [x19]
	mov w16, #0xffff
	cmp w0, w16
	b.eq .Lskip_checkpoint
	add w0, w0, #1
	strh w0, [x19]
	sub w1, w0, #1
	tst w0, w1
	b.ne .Lskip_checkpoint
	LOAD_ADDR x16, max_checkpoints
	mov x1, #MAX_CHECKPOINTS
	str x1, [x16]
	LOAD_ADDR x16, teapot_nested_root_gen
	ldr w0, [x16]
	adds w0, w0, #1
	csinc w0, w0, wzr, ne
	str w0, [x16]
	b .Lhashed_done
.Lhashed_tree:
	LOAD_ADDR x0, teapot_nested_root_gen
	ldr w0, [x0]
	ldr w16, [x17, #4]
	cmp w0, w16
	b.eq .Lhashed_generation_ready
	str w0, [x17, #4]
	str wzr, [x17, #8]
.Lhashed_generation_ready:
	ldrh w16, [x19, #8]
	mov w17, #0xffff
	cmp w16, w17
	b.eq .Lskip_checkpoint
	add w16, w16, #1
	strh w16, [x19, #8]
	cmp w16, w17
	b.eq .Lskip_checkpoint
	mov w19, w16
	lsl x0, x0, #24
	LOAD_ADDR x16, checkpoint_target_metadata
	ldr x16, [x16, #CHECKPOINT_TARGET_SITE_INDEX]
	eor x0, x0, x16, lsl #1
	eor x0, x0, x1
	LOAD_ADDR x16, teapot_nested_seed_word
	ldr x16, [x16]
	eor x0, x0, x16
	movz x17, #0xe5b9
	movk x17, #0x1ce4, lsl #16
	movk x17, #0x476d, lsl #32
	movk x17, #0xbf58, lsl #48
	mul x0, x0, x17
	eor x0, x0, x0, lsr #31
	clz w1, w19
	mov w16, #31
	sub w1, w16, w1
	movz x16, #0x11eb
	movk x16, #0x1331, lsl #16
	movk x16, #0x49bb, lsl #32
	movk x16, #0x94d0, lsl #48
	madd x0, x1, x16, x0
	eor x0, x0, x0, lsr #29
	mul x0, x0, x17
	eor x0, x0, x0, lsr #32
	mov w16, #1
	lsl w16, w16, w1
	sub w19, w19, w16
	sub w16, w16, #1
	and w0, w0, w16
	cmp w19, w0
	b.ne .Lskip_checkpoint
.Lhashed_done:
	LOAD_ADDR x16, checkpoint_cnt
	ldr x0, [x16]
