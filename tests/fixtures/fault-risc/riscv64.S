
#===================================
.section .teapot_transient ,"ax",@progbits
#===================================

.align 16

.align 16
__teapot_fault_bb_88016a5101694d4897a48ef810629257:
.L__teapot_fault_block_88016a5101694d4897a48ef810629257:
#-----------------------------------
.globl test_function
.type test_function, @function
#-----------------------------------
test_function:

.__risc_fixture_start__teapot__:
            c.nop
.align 1
.L__teapot_fault_site_0:
          .byte 0x3
          .byte 0xb5
          .byte 0x4
          .byte 0x0
.align 1
.L__teapot_fault_return_0:

.align 1

.align 1
.L__teapot_fault_site_1:
          .byte 0x83
          .byte 0xb5
          .byte 0x85
          .byte 0x0
.align 1

.align 1

.L__teapot_fault_return_1:
            jalr zero,ra,0
__teapot_fault_be_88016a5101694d4897a48ef810629257:
.L__teapot_fault_block_end_88016a5101694d4897a48ef810629257:
.size test_function, . - test_function
.align 1

.align 1
.L__teapot_fault_auipc_0_0:
.option push
.option norvc
.option norelax
__teapot_fault_rv_scope_begin_ae66a20f794150b190f8a6f7bb6c9518:
.L__teapot_fault_stub_0:

            auipc a0,%pcrel_hi(.L__teapot_fault_spill_0)
.align 1

            addi a0,a0,%pcrel_lo(.L__teapot_fault_auipc_0_0)
.align 1
          .byte 0x23
          .byte 0x30
          .byte 0x15
          .byte 0x0
          .byte 0x23
          .byte 0x34
          .byte 0x55
          .byte 0x0
          .byte 0x93
          .byte 0x80
          .byte 0x4
          .byte 0x0
.align 1
.L__teapot_fault_auipc_0_20:

            auipc t0,%pcrel_hi(teapot_fault_risc_policy)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_0_20)
.align 1
          .byte 0x83
          .byte 0xb2
          .byte 0x2
          .byte 0x0
          .byte 0x63
          .byte 0xe4
          .byte 0x50
          .byte 0x2
.align 1
.L__teapot_fault_auipc_0_36:

            auipc t0,%pcrel_hi(teapot_fault_risc_policy)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_0_36)
.align 1
          .byte 0x83
          .byte 0xb2
          .byte 0x82
          .byte 0x0
          .byte 0x63
          .byte 0x84
          .byte 0x2
          .byte 0x0
          .byte 0x63
          .byte 0xfa
          .byte 0x50
          .byte 0x0
          .byte 0x83
          .byte 0x30
          .byte 0x5
          .byte 0x0
          .byte 0x83
          .byte 0x32
          .byte 0x85
          .byte 0x0
.align 1
.L__teapot_fault_copy_0:
          .byte 0x3
          .byte 0xb5
          .byte 0x4
          .byte 0x0
.align 1
.L__teapot_fault_copy_end_0:

            jal zero,.L__teapot_fault_return_0
.align 1
.L__teapot_fault_auipc_0_72:

            auipc t0,%pcrel_hi(restore_checkpoint_SIGSEGV)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_0_72)
.align 1
          .byte 0x67
          .byte 0x80
          .byte 0x2
          .byte 0x0
.align 1

.align 1
.L__teapot_fault_stub_1:
.L__teapot_fault_auipc_1_0:

.L__teapot_fault_stub_end_0:
            auipc t1,%pcrel_hi(.L__teapot_fault_spill_1)
.align 1

            addi t1,t1,%pcrel_lo(.L__teapot_fault_auipc_1_0)
.align 1
          .byte 0x23
          .byte 0x30
          .byte 0x13
          .byte 0x0
          .byte 0x23
          .byte 0x34
          .byte 0x53
          .byte 0x0
          .byte 0x93
          .byte 0x80
          .byte 0x85
          .byte 0x0
.align 1
.L__teapot_fault_auipc_1_20:

            auipc t0,%pcrel_hi(teapot_fault_risc_policy)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_1_20)
.align 1
          .byte 0x83
          .byte 0xb2
          .byte 0x2
          .byte 0x0
          .byte 0x63
          .byte 0xe4
          .byte 0x50
          .byte 0x2
.align 1
.L__teapot_fault_auipc_1_36:

            auipc t0,%pcrel_hi(teapot_fault_risc_policy)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_1_36)
.align 1
          .byte 0x83
          .byte 0xb2
          .byte 0x82
          .byte 0x0
          .byte 0x63
          .byte 0x84
          .byte 0x2
          .byte 0x0
          .byte 0x63
          .byte 0xfa
          .byte 0x50
          .byte 0x0
          .byte 0x83
          .byte 0x30
          .byte 0x3
          .byte 0x0
          .byte 0x83
          .byte 0x32
          .byte 0x83
          .byte 0x0
.align 1
.L__teapot_fault_copy_1:
          .byte 0x83
          .byte 0xb5
          .byte 0x85
          .byte 0x0
.align 1
.L__teapot_fault_copy_end_1:

            jal zero,.L__teapot_fault_return_1
.align 1
.L__teapot_fault_auipc_1_72:

            auipc t0,%pcrel_hi(restore_checkpoint_SIGSEGV)
.align 1

            addi t0,t0,%pcrel_lo(.L__teapot_fault_auipc_1_72)
.align 1
          .byte 0x67
          .byte 0x80
          .byte 0x2
          .byte 0x0
.align 1
.L__teapot_fault_stub_end_1:
.option pop
__teapot_fault_rv_scope_end_ae66a20f794150b190f8a6f7bb6c9518:

          .zero 65354
.align 16
.__risc_fixture_end__teapot__:

#===================================
# end section .teapot_transient
#===================================

#===================================
.section teapot_protected_bss ,"wa",@nobits
#===================================

.align 3
.L__teapot_fault_counters:
          .zero 8
.L__teapot_fault_counters_end:
.align 3
.L__teapot_fault_pending:
          .zero 8
.L__teapot_fault_pending_end:
.align 3
.L__teapot_fault_spill_0:
          .zero 16
.align 3
.L__teapot_fault_spill_1:
          .zero 16
#===================================
# end section teapot_protected_bss
#===================================

#===================================
.section teapot_fault_sites ,"a",@progbits
#===================================

.align 3
.L__teapot_fault_sites:
          .byte 0x54
          .byte 0x50
          .byte 0x46
          .byte 0x53
          .byte 0x4
          .byte 0x0
          .byte 0x70
          .byte 0x0
          .byte 0x80
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x2
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x2
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x2
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_18:
          .quad .__risc_fixture_start__teapot__-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_18
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_20:
          .quad .__risc_fixture_end__teapot__-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_20
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_28:
          .quad .__risc_fixture_start__teapot__-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_28
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_30:
          .quad .__risc_fixture_end__teapot__-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_30
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_38:
          .quad .L__teapot_fault_counters-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_38
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_40:
          .quad .L__teapot_fault_counters_end-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_40
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_48:
          .quad .L__teapot_fault_pending-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_48
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_50:
          .quad .L__teapot_fault_pending_end-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_50
          .zero 24
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_70:
          .long .L__teapot_fault_site_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_70
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_74:
          .long .L__teapot_fault_stub_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_74
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_78:
          .long .L__teapot_fault_copy_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_78
          .byte 0x4
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_80:
          .long .L__teapot_fault_return_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_80
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_84:
          .long .L__teapot_fault_copy_end_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_84
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_88:
          .long .L__teapot_fault_block_88016a5101694d4897a48ef810629257-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_88
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_8c:
          .long .L__teapot_fault_block_end_88016a5101694d4897a48ef810629257-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_8c
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_90:
          .long .L__teapot_fault_spill_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_90
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_94:
          .long teapot_fault_risc_policy-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_94
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_98:
          .long restore_checkpoint_SIGSEGV-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_98
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_9c:
          .long .L__teapot_fault_stub_end_0-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_9c
          .byte 0x3
          .byte 0xb5
          .byte 0x4
          .byte 0x0
          .byte 0x1
          .byte 0x8
          .byte 0x9
          .byte 0xff
          .byte 0x0
          .byte 0x0
          .byte 0xa
          .byte 0xa
          .byte 0x1
          .byte 0x5
          .byte 0x4
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x10
          .byte 0x0
          .byte 0x1
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f0:
          .long .L__teapot_fault_site_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f0
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f4:
          .long .L__teapot_fault_stub_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f4
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f8:
          .long .L__teapot_fault_copy_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_f8
          .byte 0x4
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_100:
          .long .L__teapot_fault_return_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_100
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_104:
          .long .L__teapot_fault_copy_end_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_104
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_108:
          .long .L__teapot_fault_block_88016a5101694d4897a48ef810629257-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_108
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_10c:
          .long .L__teapot_fault_block_end_88016a5101694d4897a48ef810629257-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_10c
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_110:
          .long .L__teapot_fault_spill_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_110
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_114:
          .long teapot_fault_risc_policy-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_114
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_118:
          .long restore_checkpoint_SIGSEGV-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_118
.L__teapot_fault_field_b301ec926938425fab9e545b29384447_11c:
          .long .L__teapot_fault_stub_end_1-.L__teapot_fault_field_b301ec926938425fab9e545b29384447_11c
          .byte 0x83
          .byte 0xb5
          .byte 0x85
          .byte 0x0
          .byte 0x1
          .byte 0x8
          .byte 0xb
          .byte 0xff
          .byte 0x0
          .byte 0x0
          .byte 0xb
          .byte 0x6
          .byte 0x1
          .byte 0x5
          .byte 0x4
          .byte 0x0
          .byte 0x8
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x10
          .byte 0x0
          .byte 0x2
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
#===================================
# end section teapot_fault_sites
#===================================
#-----------------------------------
.globl restore_checkpoint_SIGSEGV
.type restore_checkpoint_SIGSEGV, @notype
#-----------------------------------
#-----------------------------------
.globl teapot_fault_risc_policy
.type teapot_fault_risc_policy, @notype
#-----------------------------------
