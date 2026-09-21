/* Real guarded-page BTI faults, using the existing checkpoint/restore path.
 * This test does not enable a hardware target-identification backend.
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
static uint64_t test_memory;
static void *fault_target;

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
    assert(simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV] == chain_signal_rollbacks + 1);
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
    chain_signal_rollbacks = simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV];
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
    assert(simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV] == chain_signal_rollbacks + 1);
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
        uint64_t saved_rollbacks = simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV];
        assert(checkpoint_bti_probe(stack_top, fault_target) == 42);
        assert(checkpoint_cnt == depth);
        assert(instruction_cnt == 17 + depth);
        assert(test_memory == UINT64_C(0x123456789abcdef));
        assert(memory_history_top == saved_history && guard_list_top == saved_guards);
        assert(!in_restore_memlog);
        for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) assert(dift_reg_tags[i] == i + 1);
        assert(simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV] == saved_rollbacks + 1);
    }
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
