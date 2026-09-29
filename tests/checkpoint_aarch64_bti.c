/* Real guarded-page BTI faults, using the existing checkpoint/restore path.
 * The default fault tests keep ordinary policy; explicit bti-backend variants
 * additionally enable and exercise the experimental target-identification path.
 */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include "checkpoint.h"
#include "signal_handler.h"

#ifndef HWCAP2_BTI
#define HWCAP2_BTI (1UL << 17)
#endif
#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif

extern int checkpoint_bti_probe(void *stack_top, void *target);
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern uint64_t max_checkpoints;
static uint64_t test_memory;
static void *fault_target;
static unsigned expected_bti_reason = ROLLBACK_SIGSEGV;
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
static bool test_active_backend;
extern void teapot_aarch64_bti_activate(void);
extern int teapot_bti_call_probe(void *);
extern char teapot_bti_test_marker[], teapot_bti_test_plain[];
extern uint64_t teapot_bti_normal_resumes, teapot_bti_rollbacks;
#endif

#ifdef ENABLE_NESTED_SPECULATION
extern int checkpoint_bti_nested_probe(void *stack_top, void *target);
static checkpoint_metadata_t outer_checkpoint;
static memory_history_t *chain_history;
static uint32_t *chain_guards;
static uint64_t chain_signal_rollbacks;
static volatile sig_atomic_t chain_faults, chain_bad_faults, chain_inner_restored;
static void (*chain_runtime_handler)(int, siginfo_t *, void *);

/* Observe the original fault, then delegate all recovery to the real handler.
 * In particular, do not clear BTYPE or replace the handler's PC redirection.
 */
static void observe_chain_bti(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (sig == SIGILL && checkpoint_cnt == 2 &&
            (info->si_code == ILL_ILLOPC || info->si_code == ILL_ILLOPN) &&
            uc->uc_mcontext.pc == (uintptr_t)fault_target && info->si_addr == fault_target)
        chain_faults++;
    else
        chain_bad_faults++;
    chain_runtime_handler(sig, info, context);
}

void checkpoint_bti_outer_mutate(void) {
    assert(checkpoint_cnt == 1);
    outer_checkpoint = checkpoint_metadata[0];
    assert(memory_history_top == chain_history && guard_list_top == chain_guards);
    *memory_history_top++ = (memory_history_t){
        .addr = &test_memory, .data = test_memory, .size = sizeof(test_memory)};
    test_memory = UINT64_C(0x1111222233334444);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = (uint8_t)(i + 17);
    instruction_cnt = 41;
    *guard_list_top++ = 0;
}

void checkpoint_bti_check_outer(int inner_result) {
    assert(inner_result == 42 && checkpoint_cnt == 1);
    assert(test_memory == UINT64_C(0x1111222233334444));
    assert(instruction_cnt == 41);
    assert(memory_history_top == chain_history + 1 && guard_list_top == chain_guards + 1);
    assert(!in_restore_memlog);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) assert(dift_reg_tags[i] == i + 17);
    assert(memcmp(&outer_checkpoint, &checkpoint_metadata[0], sizeof(outer_checkpoint)) == 0);
    assert(simulation_statistics.rollback_reason[expected_bti_reason] == chain_signal_rollbacks + 1);
    assert(chain_faults == 1 && chain_bad_faults == 0);
    chain_inner_restored = 1;
}

static void check_live_chain(void *stack_top) {
    /* Begin with no synthetic checkpoint state: assembly creates both entries. */
    checkpoint_cnt = 0;
    instruction_cnt = 17;
    test_memory = UINT64_C(0x123456789abcdef);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = (uint8_t)(i + 1);
    chain_history = memory_history_top;
    chain_guards = guard_list_top;
    chain_signal_rollbacks = simulation_statistics.rollback_reason[expected_bti_reason];
    uint64_t saved_budget_rollbacks = simulation_statistics.rollback_reason[ROLLBACK_ROB_LEN];
    uint64_t saved_outer_rollbacks = simulation_statistics.ckpt_depth[0];
    uint64_t saved_inner_rollbacks = simulation_statistics.ckpt_depth[1];
    chain_faults = chain_bad_faults = chain_inner_restored = 0;
    struct sigaction runtime_action, observed_action;
    assert(sigaction(SIGILL, NULL, &runtime_action) == 0);
    observed_action = runtime_action;
    assert(observed_action.sa_flags & SA_SIGINFO);
    chain_runtime_handler = runtime_action.sa_sigaction;
    assert(chain_runtime_handler != NULL);
    observed_action.sa_sigaction = observe_chain_bti;
    assert(sigaction(SIGILL, &observed_action, NULL) == 0);

    assert(checkpoint_bti_nested_probe(stack_top, fault_target) == 42);
    assert(checkpoint_cnt == 0 && instruction_cnt == 17);
    assert(test_memory == UINT64_C(0x123456789abcdef));
    assert(memory_history_top == chain_history && guard_list_top == chain_guards);
    assert(!in_restore_memlog);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) assert(dift_reg_tags[i] == i + 1);
    assert(chain_inner_restored == 1 && chain_faults == 1 && chain_bad_faults == 0);
    assert(simulation_statistics.rollback_reason[expected_bti_reason] == chain_signal_rollbacks + 1);
    assert(simulation_statistics.rollback_reason[ROLLBACK_ROB_LEN] == saved_budget_rollbacks + 1);
    assert(simulation_statistics.ckpt_depth[0] == saved_outer_rollbacks + 1);
    assert(simulation_statistics.ckpt_depth[1] == saved_inner_rollbacks + 1);
    assert(sigaction(SIGILL, &runtime_action, NULL) == 0);
    puts("Live checkpoint chain: depth 0 -> 1 -> 2 -> BTI rollback to 1 -> budget rollback to 0 passed");
}
#endif

static void expect_bti_signal(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    /* Linux uses ILL_ILLOPC; QEMU 10.0.11 uses ILL_ILLOPN. */
    if (sig == SIGILL && (info->si_code == ILL_ILLOPC || info->si_code == ILL_ILLOPN) &&
            uc->uc_mcontext.pc == (uintptr_t)fault_target &&
            info->si_addr == fault_target)
        _exit(0);
    _exit(3);
}

static bool negative_probe(bool through_runtime) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        checkpoint_cnt = 0;
        struct sigaction action = {.sa_sigaction = expect_bti_signal, .sa_flags = SA_SIGINFO};
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGILL, &action, NULL) == 0);
        if (through_runtime) setup_signal_handler();
        ((int (*)(void))fault_target)();
        _exit(1); /* CPU/OS accepted the deliberately invalid landing. */
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1);
    return WEXITSTATUS(status) == 0;
}

void checkpoint_bti_mutate(void) {
    assert(checkpoint_cnt > 0);
    *memory_history_top++ = (memory_history_t){
        .addr = &test_memory, .data = test_memory, .size = sizeof(test_memory)};
    test_memory = UINT64_C(0xbad00bad00);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = 0xff;
    instruction_cnt = 249;
    *guard_list_top++ = 0;
}

static int check_aarch64_bti(void *stack_top, bool live_chain) {
    if (!(getauxval(AT_HWCAP2) & HWCAP2_BTI)) {
        puts("SKIP: no HWCAP2_BTI; software target checks remain required");
        return 77;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    uint32_t *page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    page[0] = UINT32_C(0xd50324df); /* bti jc */
    page[1] = UINT32_C(0x52800540); /* mov w0, #42 */
    page[2] = UINT32_C(0xd65f03c0); /* ret */
    fault_target = page + 1;
    __builtin___clear_cache((char *)page, (char *)(page + 3));
    if (mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC | PROT_BTI)) {
        printf("SKIP: PROT_BTI rejected (errno=%d)\n", errno);
        assert(munmap(page, (size_t)page_size) == 0);
        return 77;
    }
    assert(((int (*)(void))page)() == 42);
    if (!negative_probe(false)) {
        puts("SKIP: invalid landing did not fault; no BTI enforcement demonstrated");
        assert(munmap(page, (size_t)page_size) == 0);
        return 77;
    }
    assert(mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC) == 0);
    assert(((int (*)(void))fault_target)() == 42);
    assert(mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC | PROT_BTI) == 0);
    /* Outside simulation the previous handler must see the original fault. */
    assert(negative_probe(true));

    setup_signal_handler();
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    if (test_active_backend) {
        checkpoint_cnt = 0;
        teapot_aarch64_bti_activate();
        assert(teapot_bti_call_probe(teapot_bti_test_marker) == 42);
        assert(teapot_bti_normal_resumes == 0);
        // Two calls to the exact same invalid normal target must each fault,
        // resume at PC (not PC+4), and leave protection enabled.
        assert(teapot_bti_call_probe(teapot_bti_test_plain) == 42);
        assert(teapot_bti_call_probe(teapot_bti_test_plain) == 42);
        assert(teapot_bti_normal_resumes == 2);
        fault_target = teapot_bti_test_plain;
    }
#endif
#ifdef ENABLE_NESTED_SPECULATION
    if (live_chain) {
        check_live_chain(stack_top);
        assert(munmap(page, (size_t)page_size) == 0);
        return 0;
    }
    const unsigned depths = 2;
#else
    assert(!live_chain);
    const unsigned depths = 1;
#endif
    for (unsigned depth = 0; depth < depths; depth++) {
        checkpoint_cnt = depth;
        instruction_cnt = 17 + depth;
        test_memory = UINT64_C(0x123456789abcdef);
        for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = (uint8_t)(i + 1);
        memory_history_t *saved_history = memory_history_top;
        uint32_t *saved_guards = guard_list_top;
        uint64_t saved_rollbacks = simulation_statistics.rollback_reason[expected_bti_reason];
        assert(checkpoint_bti_probe(stack_top, fault_target) == 42);
        assert(checkpoint_cnt == depth);
        assert(instruction_cnt == 17 + depth);
        assert(test_memory == UINT64_C(0x123456789abcdef));
        assert(memory_history_top == saved_history && guard_list_top == saved_guards);
        assert(!in_restore_memlog);
        for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) assert(dift_reg_tags[i] == i + 1);
        assert(simulation_statistics.rollback_reason[expected_bti_reason] == saved_rollbacks + 1);
    }
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    if (test_active_backend) {
        /* Design step 3: a speculative branch into the middle of the copy (the
         * unpadded ret word) faults and rolls back like any malformed target. */
        extern char __teapot_bti_transient_start[];
        for (unsigned depth = 0; depth < depths; depth++) {
            checkpoint_cnt = depth;
            instruction_cnt = 17 + depth;
            test_memory = UINT64_C(0x123456789abcdef);
            for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = (uint8_t)(i + 1);
            memory_history_t *saved_history = memory_history_top;
            uint32_t *saved_guards = guard_list_top;
            uint64_t saved_rollbacks = simulation_statistics.rollback_reason[expected_bti_reason];
            uint64_t saved_bti_rollbacks = teapot_bti_rollbacks;
            assert(checkpoint_bti_probe(stack_top, __teapot_bti_transient_start + 4) == 42);
            assert(memory_history_top == saved_history && guard_list_top == saved_guards);
            assert(simulation_statistics.rollback_reason[expected_bti_reason] == saved_rollbacks + 1);
            assert(teapot_bti_rollbacks == saved_bti_rollbacks + 1);
        }
        puts("Mid-copy unpadded target rollback passed");
    }
#endif
    checkpoint_cnt = 0;
    assert(munmap(page, (size_t)page_size) == 0);
    puts("BTI enforcement, forwarding and checkpoint memory/DIFT/register rollback passed");
    return 0;
}

int check_aarch64_bti_fault(void *stack_top) {
    return check_aarch64_bti(stack_top, false);
}

int check_aarch64_bti_live_chain(void *stack_top) {
#ifdef ENABLE_NESTED_SPECULATION
    return check_aarch64_bti(stack_top, true);
#else
    (void)stack_top;
    puts("SKIP: live nested BTI test requires the nested-only test binary");
    return 77;
#endif
}

#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
extern char teapot_bti_test_brk_data[], teapot_bti_test_hlt_data[];
#define TRAP_BRANCHES(kind) \
    extern char teapot_bti_test_##kind##_br0[], teapot_bti_test_##kind##_br16[], \
        teapot_bti_test_##kind##_br17[], teapot_bti_test_##kind##_blr0[], \
        teapot_bti_test_##kind##_blr16[], teapot_bti_test_##kind##_blr17[]
TRAP_BRANCHES(brk);
TRAP_BRANCHES(hlt);

static int expected_trap_signal;
static unsigned expected_btype;
static uintptr_t expected_trap_pc;

static void forwarded_trap(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    bool code_ok = sig == SIGTRAP ? info->si_code == TRAP_BRKPT :
        info->si_code == ILL_ILLOPC || info->si_code == ILL_ILLOPN;
    // In particular, neither saved BTYPE nor PC may be changed on forwarding.
    _exit(sig == expected_trap_signal && code_ok &&
        uc->uc_mcontext.pc == expected_trap_pc &&
        (uintptr_t)info->si_addr == expected_trap_pc &&
        ((uc->uc_mcontext.pstate >> 10) & 3) == expected_btype &&
        checkpoint_cnt == 0 && teapot_bti_normal_resumes == 0 &&
        teapot_bti_rollbacks == 0 ? 0 : 3);
}

int check_aarch64_bti_traps(void *stack_top) {
    if (!(getauxval(AT_HWCAP2) & HWCAP2_BTI)) return 77;
    const struct {void *branch, *target; int sig; unsigned btype;} cases[] = {
#define CASES(kind, sig) \
        {teapot_bti_test_##kind##_br0, teapot_bti_test_##kind##_data, sig, 3}, \
        {teapot_bti_test_##kind##_br16, teapot_bti_test_##kind##_data, sig, 1}, \
        {teapot_bti_test_##kind##_br17, teapot_bti_test_##kind##_data, sig, 1}, \
        {teapot_bti_test_##kind##_blr0, teapot_bti_test_##kind##_data, sig, 2}, \
        {teapot_bti_test_##kind##_blr16, teapot_bti_test_##kind##_data, sig, 2}, \
        {teapot_bti_test_##kind##_blr17, teapot_bti_test_##kind##_data, sig, 2}
        CASES(brk, SIGTRAP), CASES(hlt, SIGILL)
#undef CASES
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        // Fork before installing the parent's handler: saved dispositions must
        // not accidentally be another copy of our own handler.
        for (unsigned custom = 0; custom < 2; custom++) {
            pid_t child = fork();
            assert(child >= 0);
            if (!child) {
                checkpoint_cnt = 0;
                expected_trap_signal = cases[i].sig;
                expected_trap_pc = (uintptr_t)cases[i].target;
                expected_btype = cases[i].btype;
                struct sigaction action = {.sa_sigaction = forwarded_trap,
                                           .sa_flags = SA_SIGINFO};
                if (!custom) action = (struct sigaction){.sa_handler = SIG_DFL};
                sigemptyset(&action.sa_mask);
                assert(sigaction(cases[i].sig, &action, NULL) == 0);
                setup_signal_handler();
                teapot_aarch64_bti_activate();
                teapot_bti_call_probe(cases[i].branch);
                _exit(1);
            }
            int status;
            assert(waitpid(child, &status, 0) == child);
            if (custom) assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            else assert(WIFSIGNALED(status) && WTERMSIG(status) == cases[i].sig);
        }
    }
    setup_signal_handler();
    teapot_aarch64_bti_activate();
#ifdef ENABLE_NESTED_SPECULATION
    const unsigned depths = 2;
#else
    const unsigned depths = 1;
#endif
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    for (unsigned depth = 0; depth < depths; depth++) {
        checkpoint_cnt = depth;
        // Repeated depth-zero calls can lower the next nested capacity through
        // the scheduling heuristic. Explicitly exercise each requested depth.
        max_checkpoints = MAX_CHECKPOINTS;
        instruction_cnt = 17 + depth;
        test_memory = UINT64_C(0x123456789abcdef);
        for (size_t j = 0; j < DIFT_REG_TAGS_SIZE; j++) dift_reg_tags[j] = (uint8_t)(j + 1);
        memory_history_t *saved_history = memory_history_top;
        uint32_t *saved_guards = guard_list_top;
        uint64_t saved_rollbacks = simulation_statistics.rollback_reason[ROLLBACK_MALFORMED_INDIRECT_BR];
        uint64_t saved_bti_rollbacks = teapot_bti_rollbacks;
        assert(checkpoint_bti_probe(stack_top, cases[i].branch) == 42);
        assert(checkpoint_cnt == depth && instruction_cnt == 17 + depth);
        assert(test_memory == UINT64_C(0x123456789abcdef));
        assert(memory_history_top == saved_history && guard_list_top == saved_guards);
        assert(!in_restore_memlog);
        for (size_t j = 0; j < DIFT_REG_TAGS_SIZE; j++) assert(dift_reg_tags[j] == j + 1);
        assert(simulation_statistics.rollback_reason[ROLLBACK_MALFORMED_INDIRECT_BR] == saved_rollbacks + 1);
        assert(teapot_bti_rollbacks == saved_bti_rollbacks + 1);
        assert(teapot_bti_normal_resumes == 0);
    }
    checkpoint_cnt = 0;
    puts("BRK/HLT data landings: 24 ordinary forwarding cases and all checkpoint restores passed");
    return 0;
}

int check_aarch64_bti_backend(void *stack_top, bool live_chain) {
#ifndef ENABLE_NESTED_SPECULATION
    if (live_chain) return 77;
#endif
    test_active_backend = true;
    expected_bti_reason = ROLLBACK_MALFORMED_INDIRECT_BR;
    int result = check_aarch64_bti(stack_top, live_chain);
    if (!result) {
#ifdef ENABLE_NESTED_SPECULATION
        /* Each depth takes the new mid-copy loop rollback; the live chain
         * returns before that loop. */
        assert(teapot_bti_rollbacks == (live_chain ? 1 : 4));
#else
        assert(teapot_bti_rollbacks == 2);
#endif
        assert(teapot_bti_normal_resumes == 2);
        puts("Active BTI backend: same-PC normal resume and malformed-target rollback passed");
    }
    return result;
}
#endif
