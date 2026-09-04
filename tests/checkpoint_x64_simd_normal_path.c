#include <stdbool.h>
#include <stdint.h>

extern bool libcheckpoint_enabled;
extern uint64_t checkpoint_cnt;
extern uint64_t processor_xsave_mask;

extern int checkpoint_x64_simd_normal_path_probe(void);

/* Normal instrumented links provide these coverage-section boundary symbols. */
uint32_t __guard_start__teapot__[1];
uint32_t __guard_end__teapot__[1];

/* Override libcheckpoint's weak coverage hook with a deliberate XMM clobber. */
__attribute__((noinline)) void hfuzz_trace_pc(uint64_t pc) {
    (void)pc;
    __asm__ volatile("pxor %%xmm0, %%xmm0" ::: "xmm0");
}

int main(void) {
    /* Exercise the portable XMM fallback without requiring runtime startup. */
    processor_xsave_mask = 0;
    checkpoint_cnt = 0;
    libcheckpoint_enabled = true;
    return checkpoint_x64_simd_normal_path_probe() ? 0 : 1;
}
