#===================================
.arch armv8-a
#===================================


#===================================
.section .teapot_transient ,"ax",@progbits
#===================================

.align 16
.__risc_fixture_start__teapot__:

.align 16
__teapot_fault_bb_f14bd7867ce84228bd8fad20a4f2b989:
.L__teapot_fault_block_f14bd7867ce84228bd8fad20a4f2b989:
#-----------------------------------
.globl test_function
.type test_function, @function
#-----------------------------------
test_function:

.align 2
.L__teapot_fault_site_0:
          .byte 0x41
          .byte 0x4
          .byte 0x40
          .byte 0xf9
.align 2

.align 2
.L__teapot_fault_return_0:

.align 2
.L__teapot_fault_site_1:
          .byte 0x21
          .byte 0x4
          .byte 0x40
          .byte 0xf9
.align 2
.L__teapot_fault_return_1:

.align 2

            ret 
__teapot_fault_be_f14bd7867ce84228bd8fad20a4f2b989:
.L__teapot_fault_block_end_f14bd7867ce84228bd8fad20a4f2b989:
.size test_function, . - test_function
.align 2

.align 2
.L__teapot_fault_stub_0:

            adrp x1, .L__teapot_fault_spill_0
.align 2

            add x1,x1, :lo12:.L__teapot_fault_spill_0
.align 2
          .byte 0x20
          .byte 0x0
          .byte 0x0
          .byte 0xf9
          .byte 0x23
          .byte 0x4
          .byte 0x0
          .byte 0xf9
          .byte 0x0
          .byte 0x42
          .byte 0x3b
          .byte 0xd5
          .byte 0x20
          .byte 0x8
          .byte 0x0
          .byte 0xf9
          .byte 0x40
          .byte 0x20
          .byte 0x0
          .byte 0x91
.align 2

            adrp x3, teapot_fault_risc_policy
.align 2
          .byte 0x0
          .byte 0xdc
          .byte 0x40
          .byte 0xd3
.align 2

            add x3,x3, :lo12:teapot_fault_risc_policy
.align 2
          .byte 0x63
          .byte 0x0
          .byte 0x40
          .byte 0xf9
          .byte 0x1f
          .byte 0x0
          .byte 0x3
          .byte 0xeb
          .byte 0xe3
          .byte 0x0
          .byte 0x0
          .byte 0x54
          .byte 0x20
          .byte 0x8
          .byte 0x40
          .byte 0xf9
          .byte 0x0
          .byte 0x42
          .byte 0x1b
          .byte 0xd5
          .byte 0x20
          .byte 0x0
          .byte 0x40
          .byte 0xf9
          .byte 0x23
          .byte 0x4
          .byte 0x40
          .byte 0xf9
.align 2
.L__teapot_fault_copy_0:
          .byte 0x41
          .byte 0x4
          .byte 0x40
          .byte 0xf9
.align 2
.L__teapot_fault_copy_end_0:

            b .L__teapot_fault_return_0
.align 2

            b restore_checkpoint_SIGSEGV
.align 2
.L__teapot_fault_stub_end_0:

.align 2
.L__teapot_fault_stub_1:

            adrp x17, .L__teapot_fault_spill_1
.align 2

            add x17,x17, :lo12:.L__teapot_fault_spill_1
.align 2
          .byte 0x20
          .byte 0x2
          .byte 0x0
          .byte 0xf9
          .byte 0x22
          .byte 0x6
          .byte 0x0
          .byte 0xf9
          .byte 0x0
          .byte 0x42
          .byte 0x3b
          .byte 0xd5
          .byte 0x20
          .byte 0xa
          .byte 0x0
          .byte 0xf9
          .byte 0x20
          .byte 0x20
          .byte 0x0
          .byte 0x91
.align 2

            adrp x2, teapot_fault_risc_policy
.align 2
          .byte 0x0
          .byte 0xdc
          .byte 0x40
          .byte 0xd3
.align 2

            add x2,x2, :lo12:teapot_fault_risc_policy
.align 2
          .byte 0x42
          .byte 0x0
          .byte 0x40
          .byte 0xf9
          .byte 0x1f
          .byte 0x0
          .byte 0x2
          .byte 0xeb
          .byte 0xe3
          .byte 0x0
          .byte 0x0
          .byte 0x54
          .byte 0x20
          .byte 0xa
          .byte 0x40
          .byte 0xf9
          .byte 0x0
          .byte 0x42
          .byte 0x1b
          .byte 0xd5
          .byte 0x20
          .byte 0x2
          .byte 0x40
          .byte 0xf9
          .byte 0x22
          .byte 0x6
          .byte 0x40
          .byte 0xf9
.align 2
.L__teapot_fault_copy_1:
          .byte 0x21
          .byte 0x4
          .byte 0x40
          .byte 0xf9
.align 2
.L__teapot_fault_copy_end_1:

            b .L__teapot_fault_return_1
.align 2

            b restore_checkpoint_SIGSEGV
.align 2
.L__teapot_fault_stub_end_1:

          .zero 65364
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
          .zero 24
.align 3
.L__teapot_fault_spill_1:
          .zero 24
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
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_18:
          .quad .__risc_fixture_start__teapot__-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_18
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_20:
          .quad .__risc_fixture_end__teapot__-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_20
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_28:
          .quad .__risc_fixture_start__teapot__-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_28
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_30:
          .quad .__risc_fixture_end__teapot__-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_30
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_38:
          .quad .L__teapot_fault_counters-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_38
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_40:
          .quad .L__teapot_fault_counters_end-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_40
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_48:
          .quad .L__teapot_fault_pending-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_48
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_50:
          .quad .L__teapot_fault_pending_end-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_50
          .zero 24
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_70:
          .long .L__teapot_fault_site_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_70
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_74:
          .long .L__teapot_fault_stub_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_74
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_78:
          .long .L__teapot_fault_copy_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_78
          .byte 0x4
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_80:
          .long .L__teapot_fault_return_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_80
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_84:
          .long .L__teapot_fault_copy_end_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_84
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_88:
          .long .L__teapot_fault_block_f14bd7867ce84228bd8fad20a4f2b989-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_88
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_8c:
          .long .L__teapot_fault_block_end_f14bd7867ce84228bd8fad20a4f2b989-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_8c
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_90:
          .long .L__teapot_fault_spill_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_90
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_94:
          .long teapot_fault_risc_policy-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_94
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_98:
          .long restore_checkpoint_SIGSEGV-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_98
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_9c:
          .long .L__teapot_fault_stub_end_0-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_9c
          .byte 0x41
          .byte 0x4
          .byte 0x40
          .byte 0xf9
          .byte 0x1
          .byte 0x8
          .byte 0x2
          .byte 0xff
          .byte 0x0
          .byte 0x0
          .byte 0x1
          .byte 0x1
          .byte 0x0
          .byte 0x3
          .byte 0x1
          .byte 0x0
          .byte 0x8
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x18
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
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f0:
          .long .L__teapot_fault_site_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f0
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f4:
          .long .L__teapot_fault_stub_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f4
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f8:
          .long .L__teapot_fault_copy_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_f8
          .byte 0x4
          .byte 0x0
          .byte 0x0
          .byte 0x0
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_100:
          .long .L__teapot_fault_return_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_100
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_104:
          .long .L__teapot_fault_copy_end_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_104
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_108:
          .long .L__teapot_fault_block_f14bd7867ce84228bd8fad20a4f2b989-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_108
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_10c:
          .long .L__teapot_fault_block_end_f14bd7867ce84228bd8fad20a4f2b989-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_10c
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_110:
          .long .L__teapot_fault_spill_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_110
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_114:
          .long teapot_fault_risc_policy-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_114
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_118:
          .long restore_checkpoint_SIGSEGV-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_118
.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_11c:
          .long .L__teapot_fault_stub_end_1-.L__teapot_fault_field_c8f03a132c5e42b5895b4609475fde74_11c
          .byte 0x21
          .byte 0x4
          .byte 0x40
          .byte 0xf9
          .byte 0x1
          .byte 0x8
          .byte 0x1
          .byte 0xff
          .byte 0x0
          .byte 0x0
          .byte 0x1
          .byte 0x11
          .byte 0x0
          .byte 0x2
          .byte 0x1
          .byte 0x0
          .byte 0x8
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x0
          .byte 0x18
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
