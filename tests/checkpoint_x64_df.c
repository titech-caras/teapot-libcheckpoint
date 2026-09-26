#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include "checkpoint.h"

extern uint64_t max_checkpoints;
extern uintptr_t checkpoint_target_metadata[];
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern void checkpoint_df_probe(uint64_t flags);
uint64_t checkpoint_df_seen[8];
static uint32_t df_branch_count;

extern __attribute__((noreturn)) void __real_restore_checkpoint(int reason);
__attribute__((noreturn)) void __wrap_restore_checkpoint(int reason) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cld" : "=r"(flags) :: "cc");
    assert(!(flags & 0x400));
    __real_restore_checkpoint(reason);
}

/* Read DF before any C library call, then repair the ABI even on a red test. */
void hfuzz_trace_pc(uint64_t pc) {
    (void)pc;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cld" : "=r"(flags) :: "cc");
    checkpoint_df_seen[6] = flags;
    checkpoint_df_seen[7]++;
}

void check_x64_df(void) {
    const uint64_t mask = 0xcd5; /* OF, DF, SF, ZF, AF, PF, CF */
    const uint64_t values[] = {0, 0x400, 0x8d5, 0xcd5};
    for (unsigned skip = 0; skip < 3; ++skip) {
        for (unsigned depth = 0; depth < MAX_CHECKPOINTS; ++depth) {
            for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
                checkpoint_cnt = skip == 2 ? MAX_CHECKPOINTS : depth;
                max_checkpoints = MAX_CHECKPOINTS;
                libcheckpoint_enabled = skip != 1;
                checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR / 8] =
                    (uintptr_t)&df_branch_count;
                df_branch_count = 0;
                memory_history_top = memory_history;
                guard_list_top = guard_list;
                for (unsigned j = 0; j < 8; ++j) checkpoint_df_seen[j] = 0;
                checkpoint_df_probe(values[i]);
                assert((checkpoint_df_seen[0] & mask) == values[i]);
                assert(checkpoint_df_seen[2] == 0x12345678); /* RBX */
                assert(checkpoint_df_seen[3] == 0x23456789); /* RCX */
                assert(checkpoint_df_seen[4] == 0x34567890); /* RDX */
                assert(checkpoint_df_seen[5] == 0x45678901); /* red zone */
                if (!skip) {
                    assert((checkpoint_df_seen[1] & mask) == values[i]);
                    assert(checkpoint_cnt == depth);
                    if (depth == 0) {
                        assert(checkpoint_df_seen[7] == 1);
                        assert(!(checkpoint_df_seen[6] & 0x400));
                    }
                } else {
                    assert(checkpoint_df_seen[7] == 0);
                }
            }
        }
    }
    checkpoint_cnt = 0;
}
