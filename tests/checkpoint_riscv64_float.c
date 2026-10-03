#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "checkpoint.h"

extern uint64_t max_checkpoints;
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern void checkpoint_float_probe(void);
/* f8, f31 and FCSR on the restored path, and whether the speculative path ran. */
uint64_t checkpoint_float_seen[4];

/* Exit 3 when the rollback leaves the speculative FP state: CTest's WILL_FAIL
 * expects that from a runtime built without the FP state, and a crash (a
 * vacuous run, below) fails whatever WILL_FAIL says. */
void check_riscv64_float(void) {
    checkpoint_cnt = 0;
    max_checkpoints = MAX_CHECKPOINTS;
    memory_history_top = memory_history;
    guard_list_top = guard_list;
    checkpoint_float_probe();
    assert(checkpoint_float_seen[3] == 1);  /* the speculative path ran */
    assert(checkpoint_cnt == 0);
    if (checkpoint_float_seen[0] != UINT64_C(0x400921fb54442d18) ||
            checkpoint_float_seen[1] != UINT64_C(0x4005bf0a8b145769) || checkpoint_float_seen[2] != 0x23) {
        fprintf(stderr, "riscv64-float: the rollback left f8=%#" PRIx64 " f31=%#" PRIx64 " fcsr=%#" PRIx64 "\n",
                checkpoint_float_seen[0], checkpoint_float_seen[1], checkpoint_float_seen[2]);
        exit(3);
    }
}
