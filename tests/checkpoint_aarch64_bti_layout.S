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
__teapot_bti_text_end:
    .section .teapot_transient,"ax",%progbits
    .p2align 2
    .global __teapot_bti_transient_start, __teapot_bti_transient_end
__teapot_bti_transient_start:
    nop
    ret
__teapot_bti_transient_end:
    .section .note.GNU-stack,"",%progbits
