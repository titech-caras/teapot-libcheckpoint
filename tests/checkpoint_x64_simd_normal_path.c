#include <stdbool.h>
#include <stdint.h>
#include <cpuid.h>
#include <assert.h>
#include "checkpoint.h"

extern uint64_t libcheckpoint_enabled;
extern uint64_t checkpoint_cnt;
extern uint64_t processor_xsave_mask;

extern int checkpoint_x64_simd_normal_path_probe(void);

/* Normal instrumented links provide these coverage-section boundary symbols. */
uint32_t __guard_start__teapot__[1];
uint32_t __guard_end__teapot__[1];

/* Override libcheckpoint's weak coverage hook with a deliberate XMM clobber. */
__attribute__((noinline)) void hfuzz_trace_pc(uint64_t pc) {
    (void)pc;
    __asm__ volatile("pxor %%xmm0, %%xmm0; pxor %%xmm15, %%xmm15" ::: "xmm0", "xmm15");
}

int main(void) {
    /* Exercise the portable XMM fallback without requiring runtime startup. */
    processor_xsave_mask = 0;
    checkpoint_cnt = 0;
    unsigned eax, ebx, ecx, edx;
    uint64_t mask = 0, optimized = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_OSXSAVE)) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        mask = (((uint64_t)hi << 32) | lo) & 7;
        __cpuid_count(0x0d, 1, eax, ebx, ecx, edx);
        optimized = eax & 1;
    }
    for (unsigned path = 0; path < 3; ++path) {
        processor_xsave_mask = path ? mask : 0;
        processor_has_xsaveopt = path == 2 ? optimized : 0;
        for (unsigned mode = 1; mode <= 4; ++mode) {
            libcheckpoint_enabled = false;
            libcheckpoint_set_vector_state(mode);
            libcheckpoint_enabled = true;
            assert(checkpoint_x64_simd_normal_path_probe());
        }
    }
    return 0;
}
