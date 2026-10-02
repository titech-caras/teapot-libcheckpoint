#include "checkpoint.h"

#if defined(__aarch64__)
#define ELF_NOBITS %nobits
#define ELF_PROGBITS %progbits
#define ELF_OBJECT %object
#else
#define ELF_NOBITS @nobits
#define ELF_PROGBITS @progbits
#define ELF_OBJECT @object
#endif

	.section teapot_protected_bss,"aw",ELF_NOBITS

	.macro RESERVE name, size, alignment
	.balign \alignment
	.globl \name
	.type \name, ELF_OBJECT
\name:
	.zero \size
	.size \name, . - \name
	.endm

	RESERVE memory_history, MEM_HISTORY_LEN*MEM_HISTORY_ENTRY_SIZE, 16
	/* Rollback targets and the depth limit: runtime state like the rest. */
	RESERVE checkpoint_target_metadata, CHECKPOINT_TARGET_METADATA_SIZE, 16
	RESERVE guard_list, GUARD_LIST_LEN*4, 16
	RESERVE scratchpad, SCRATCHPAD_SIZE, 16
	/* Last, so its 8-byte alignment leaves no gap between the objects. */
	RESERVE max_checkpoints, 8, 8

	.section .note.GNU-stack,"",ELF_PROGBITS
