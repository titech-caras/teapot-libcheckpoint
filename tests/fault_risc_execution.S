/* Focused guard wrapper, not a replacement checkpoint/rollback implementation. */
#if defined(__aarch64__)
.text
.balign 4
.global fault_risc_execute
.type fault_risc_execute,%function
fault_risc_execute:
#ifdef FAULT_RISC_TEST_BTI_PAC
    paciasp
#endif
    stp x19,x30,[sp,#-32]!
    str x20,[sp,#16]
    mov x19,x1
    mov x20,sp
    str x18,[x19,#64]
    str x29,[x19,#80]
    str x20,[x19,#56]
    mov x4,x2
    mov x2,x0
    mov x3,#0x333
    mov x17,#0x777
    msr nzcv,x4
    bl test_function
    str x1,[x19,#0]
    str x0,[x19,#8]
    str x2,[x19,#16]
    str x3,[x19,#24]
    str x17,[x19,#32]
    mrs x5,nzcv
    str x5,[x19,#40]
    mov x5,sp
    str x5,[x19,#48]
    str x18,[x19,#72]
    str x29,[x19,#88]
    ldr x20,[sp,#16]
    ldp x19,x30,[sp],#32
#ifdef FAULT_RISC_TEST_BTI_PAC
    autiasp
#endif
    ret
.size fault_risc_execute,.-fault_risc_execute
#ifdef FAULT_RISC_TEST_BTI_PAC
.global fault_risc_bad_landing
.type fault_risc_bad_landing,%function
fault_risc_bad_landing:
    br x0
.size fault_risc_bad_landing,.-fault_risc_bad_landing
#endif
.section .note.GNU-stack,"",%progbits
#elif defined(__riscv) && __riscv_xlen == 64
.text
.option push
.option norelax
.balign 2
.global fault_risc_execute
.type fault_risc_execute,@function
fault_risc_execute:
    addi sp,sp,-48
    sd ra,40(sp)
    sd s1,32(sp)
    sd s2,24(sp)
    mv s1,a0
    mv s2,a1
    sd sp,56(s2)
    sd gp,64(s2)
    sd tp,80(s2)
    sd s0,96(s2)
    mv a1,a0
    li t0,0x555
    li t1,0x666
    call test_function
    sd a0,0(s2)
    sd a1,8(s2)
    sd t0,16(s2)
    sd t1,24(s2)
    sd s1,32(s2)
    sd sp,48(s2)
    sd gp,72(s2)
    sd tp,88(s2)
    sd s0,104(s2)
    ld s2,24(sp)
    ld s1,32(sp)
    ld ra,40(sp)
    addi sp,sp,48
    ret
.size fault_risc_execute,.-fault_risc_execute
.option pop
.section .note.GNU-stack,"",@progbits
#else
#error RISC execution fixture requires a supported RISC ISA
#endif
