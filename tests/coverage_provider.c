/*
 * The coverage provider of the runtime tests, in place of libhfuzz.
 *
 * A COVERAGE runtime references hfuzz_trace_pc (the checkpoint entry's
 * normal-path hook), __sanitizer_cov_trace_pc_guard_init and
 * __sanitizer_cov_trace_pc_guard as ordinary undefined symbols and defines no
 * fallback, so a test program that compiles or links a COVERAGE runtime must
 * provide all three. Each test program built that way compiles this file, which
 * records every call in coverage_provider_calls. A test that checks a callback
 * itself defines that one and leaves it out here with
 * COVERAGE_PROVIDER_WITHOUT_TRACE_PC or COVERAGE_PROVIDER_WITHOUT_GUARD.
 *
 * These are the tests' providers, not a production fallback: a fuzzing build
 * links libhfuzz (README.md).
 */
#include <stdint.h>

struct coverage_provider_calls {
    uint64_t trace_pc, guard_init, guard;
    uint64_t last_pc;
    uint32_t *last_start, *last_stop, *last_guard;
} coverage_provider_calls;

#ifndef COVERAGE_PROVIDER_WITHOUT_TRACE_PC
void hfuzz_trace_pc(uint64_t pc) {
    coverage_provider_calls.trace_pc++;
    coverage_provider_calls.last_pc = pc;
}
#endif

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    coverage_provider_calls.guard_init++;
    coverage_provider_calls.last_start = start;
    coverage_provider_calls.last_stop = stop;
}

#ifndef COVERAGE_PROVIDER_WITHOUT_GUARD
void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    coverage_provider_calls.guard++;
    coverage_provider_calls.last_guard = guard;
}
#endif
