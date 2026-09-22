    .text
    .p2align 2
    .global teapot_bti_call_probe
    .hidden teapot_bti_call_probe
    .type teapot_bti_call_probe,%function
teapot_bti_call_probe:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    blr x0
    ldp x29, x30, [sp], #16
    ret
    .size teapot_bti_call_probe, .-teapot_bti_call_probe

    // Guarded, but outside the application target interval.
    .section .teapot_bti_probe,"ax",%progbits
    .p2align 2
    .global teapot_bti_probe_valid, teapot_bti_probe_invalid
    .hidden teapot_bti_probe_valid, teapot_bti_probe_invalid
teapot_bti_probe_valid:
    .word 0xd50324df
teapot_bti_probe_invalid:
    mov w0, #42
    ret
    .section .note.GNU-stack,"",%progbits
