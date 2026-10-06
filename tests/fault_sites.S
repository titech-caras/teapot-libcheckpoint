/* Real patchable accesses and exact copies for metadata tests only.
 * No guards, publishers, overwritten instructions or adaptive branches. */
#include "fault_sites.h"
#if defined(__riscv)
.option push
.option norvc
#endif
.macro access name
\name:
#if defined(__x86_64__)
    /* Force disp32: identical load, seven bytes, enough for JMP rel32. */
    .byte 0x48, 0x8b, 0x87
    .4byte 0
#elif defined(__aarch64__)
    ldr x0, [x0]
#else
    ld a0, 0(a0)
#endif
\name\()_end:
    ret
.balign 8
.endm
.section .text.fault_test,"ax",%progbits
.balign 8
.global fault_text_start, fault_pc_first, fault_pc_last, fault_text_end, fault_copy_first, fault_copy_last
fault_text_start:
access fault_pc_first
access fault_pc_last
/* Deliberately reversed: copy-PC order is independent of original-PC order. */
access fault_copy_last
access fault_copy_first
fault_text_end:
.balign 8
.global fault_pc_threshold1, fault_pc_threshold255, fault_pc_threshold0
access fault_pc_threshold1
access fault_copy_threshold1
fault_threshold1_end:
access fault_pc_threshold255
access fault_copy_threshold255
fault_threshold255_end:
access fault_pc_threshold0
access fault_copy_threshold0
fault_threshold0_end:
.section teapot_protected_bss,"aw",%nobits
.balign 8
fault_counters: .zero 8
fault_counters_end:
fault_pending: .zero 8
fault_pending_end:
.macro fault_storage name
.balign 8
\name\()_counters: .zero 8
\name\()_counters_end:
\name\()_pending: .zero 8
\name\()_pending_end:
.endm
fault_storage fault_one
fault_storage fault_max
fault_storage fault_zero
.section teapot_fault_sites,"a",%progbits
.balign 8
.global fault_test_table
fault_test_table:
.4byte TEAPOT_FAULT_SITE_MAGIC
.2byte TEAPOT_FAULT_SITE_VERSION, TEAPOT_FAULT_SITE_HEADER_SIZE
.4byte TEAPOT_FAULT_SITE_ENTRY_SIZE, 2, TEAPOT_FAULT_SITE_TRAINING_ONLY, 2
.8byte fault_text_start - .
.8byte fault_text_end - .
.8byte fault_copy_last - .
.8byte fault_text_end - .
.8byte fault_counters - .
.8byte fault_counters_end - .
.8byte fault_pending - .
.8byte fault_pending_end - .
.8byte 0, 0, 0
.4byte fault_pc_first - .
.4byte fault_copy_first - .
.4byte fault_copy_first - .
.2byte fault_pc_first_end - fault_pc_first, 0
.4byte fault_pc_last - .
.4byte fault_copy_last - .
.4byte fault_copy_last - .
.2byte fault_pc_last_end - fault_pc_last, 0
.macro fault_table name, pc, copy, end, storage, threshold
.balign 8
.global \name
\name:
.4byte TEAPOT_FAULT_SITE_MAGIC
.2byte TEAPOT_FAULT_SITE_VERSION, TEAPOT_FAULT_SITE_HEADER_SIZE
.4byte TEAPOT_FAULT_SITE_ENTRY_SIZE, 1, TEAPOT_FAULT_SITE_TRAINING_ONLY, \threshold
.8byte \pc - .
.8byte \end - .
.8byte \copy - .
.8byte \end - .
.8byte \storage\()_counters - .
.8byte \storage\()_counters_end - .
.8byte \storage\()_pending - .
.8byte \storage\()_pending_end - .
.8byte 0, 0, 0
.4byte \pc - .
.4byte \copy - .
.4byte \copy - .
.2byte \pc\()_end - \pc, 0
.endm
fault_table fault_one_table, fault_pc_threshold1, fault_copy_threshold1, fault_threshold1_end, fault_one, 1
fault_table fault_max_table, fault_pc_threshold255, fault_copy_threshold255, fault_threshold255_end, fault_max, 255
fault_table fault_zero_table, fault_pc_threshold0, fault_copy_threshold0, fault_threshold0_end, fault_zero, 0
#if defined(__riscv)
.option pop
#endif
.section .note.GNU-stack,"",%progbits
