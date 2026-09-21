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

int check_aarch64_bti_fault(void *stack_top) {
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
    const unsigned depths = 2;
#else
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
