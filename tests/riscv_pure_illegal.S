/* Independent literal encodings: never derive a negative from the decoder.
 * .insn marks code (not $d data), even under the approved non-F/D ISA. */

    .text
    .option push
    .option norvc
    .option norelax
#ifdef TEST_PURE_FCSR_ONLY
    .globl teapot_pure_static_fcsr
    .type teapot_pure_static_fcsr,@function
teapot_pure_static_fcsr:
    .insn 4, 0x00301073  /* csrrw zero,fcsr,zero: STATIC negative only */
    ret
    .size teapot_pure_static_fcsr, .-teapot_pure_static_fcsr
#else
    .globl teapot_pure_illegal_reserved
    .type teapot_pure_illegal_reserved,@function
teapot_pure_illegal_reserved:
    .insn 2, 0x0000  /* reserved compressed instruction */
    ret
    .size teapot_pure_illegal_reserved, .-teapot_pure_illegal_reserved
    .globl teapot_pure_illegal_fadd_s
    .type teapot_pure_illegal_fadd_s,@function
teapot_pure_illegal_fadd_s:
    .insn 4, 0x00000053  /* fadd.s f0,f0,f0 */
    ret
    .size teapot_pure_illegal_fadd_s, .-teapot_pure_illegal_fadd_s
    .globl teapot_pure_illegal_fadd_d
    .type teapot_pure_illegal_fadd_d,@function
teapot_pure_illegal_fadd_d:
    .insn 4, 0x02000053  /* fadd.d f0,f0,f0 */
    ret
    .size teapot_pure_illegal_fadd_d, .-teapot_pure_illegal_fadd_d
    .globl teapot_pure_illegal_fmadd_d
    .type teapot_pure_illegal_fmadd_d,@function
teapot_pure_illegal_fmadd_d:
    .insn 4, 0x02000043  /* fmadd.d f0,f0,f0,f0 */
    ret
    .size teapot_pure_illegal_fmadd_d, .-teapot_pure_illegal_fmadd_d
#endif
    .option pop
    .section .note.GNU-stack,"",@progbits
