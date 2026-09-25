    .section .teapot_bti_normal,"ax",%progbits
    .p2align 2
    .global __teapot_bti_text_start, __teapot_bti_text_end
    .global teapot_bti_test_marker, teapot_bti_test_plain
__teapot_bti_text_start:
teapot_bti_test_marker:
    .word 0xd50324df
    .word 0xd280a29f
teapot_bti_test_plain:
    mov w0, #42
    ret
    // Model data embedded in guarded text. Do not outline or change its bytes.
    .global teapot_bti_test_brk_data, teapot_bti_test_hlt_data
    .type teapot_bti_test_brk_data,%object
teapot_bti_test_brk_data:
    .word 0xd427a7c0 // OpenSSL table word: brk #0x3d3e
    .size teapot_bti_test_brk_data, 4
    .type teapot_bti_test_hlt_data,%object
teapot_bti_test_hlt_data:
    .word 0xd447a7c0 // hlt #0x3d3e
    .size teapot_bti_test_hlt_data, 4

    .macro trap_branch kind, reg, link, suffix
    .global teapot_bti_test_\kind\()_\suffix
teapot_bti_test_\kind\()_\suffix:
    .word 0xd50324df
    .word 0xd280a29f
    adr \reg, teapot_bti_test_\kind\()_data
    .if \link
    blr \reg
    .else
    br \reg
    .endif
    // An admitted target that executes past the trap is an immediate failure.
    brk #1
    .endm
    trap_branch brk, x0, 0, br0
    trap_branch brk, x16, 0, br16
    trap_branch brk, x17, 0, br17
    trap_branch brk, x0, 1, blr0
    trap_branch brk, x16, 1, blr16
    trap_branch brk, x17, 1, blr17
    trap_branch hlt, x0, 0, br0
    trap_branch hlt, x16, 0, br16
    trap_branch hlt, x17, 0, br17
    trap_branch hlt, x0, 1, blr0
    trap_branch hlt, x16, 1, blr16
    trap_branch hlt, x17, 1, blr17
__teapot_bti_text_end:
    .section .teapot_transient,"ax",%progbits
    .p2align 2
    .global __teapot_bti_transient_start, __teapot_bti_transient_end
__teapot_bti_transient_start:
    nop
    ret
__teapot_bti_transient_end:
    .section .note.GNU-stack,"",%progbits
