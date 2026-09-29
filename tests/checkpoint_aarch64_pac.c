/* PAC activation and FPAC authentication-fault handling.
 *
 * Depth zero must forward the original SIGILL unchanged; inside simulation the
 * same fault must roll back through the malformed-target restore path with its
 * own counter. FPAC absence is reported, not silently treated as success: the
 * rewriter's software return predicate remains the backstop there.
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
#include <sys/wait.h>
#include <unistd.h>

#include "checkpoint.h"
#include "signal_handler.h"

#ifndef HWCAP_PACA
#define HWCAP_PACA (1UL << 30)
#endif

extern int checkpoint_pac_probe(void *stack_top, void *target);
extern void teapot_pac_bad_auth(void);
extern memory_history_t *memory_history_top;
extern uint32_t *guard_list_top;
extern uint64_t max_checkpoints;
extern uint64_t teapot_pac_rollbacks;
extern void teapot_aarch64_pac_activate(void);
extern uint64_t teapot_aarch64_pac_fpac(void);
static uint64_t test_memory;

void checkpoint_pac_mutate(void) {
    assert(checkpoint_cnt > 0);
    *memory_history_top++ = (memory_history_t){
        .addr = &test_memory, .data = test_memory, .size = sizeof(test_memory)};
    test_memory = UINT64_C(0xbad00bad00);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = 0xff;
    instruction_cnt = 249;
    *guard_list_top++ = 0;
}

static void forwarded_pac(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    uintptr_t bad = (uintptr_t)teapot_pac_bad_auth;
    /* The original fault must arrive unchanged: same PC inside the
     * authentication sequence, PAC signal code, depth zero, no rollback. */
    _exit(sig == SIGILL && (info->si_code == ILL_ILLOPN || info->si_code == ILL_ILLOPC) &&
        uc->uc_mcontext.pc >= bad && uc->uc_mcontext.pc < bad + 28 &&
        checkpoint_cnt == 0 && teapot_pac_rollbacks == 0 ? 0 : 3);
}

int check_aarch64_pac(void *stack_top) {
    if (!(getauxval(AT_HWCAP) & HWCAP_PACA)) {
        puts("SKIP: no HWCAP_PACA; the software return predicate remains required");
        return 77;
    }
    setup_signal_handler();
    teapot_aarch64_pac_activate();
    teapot_aarch64_pac_activate(); /* idempotent */
    if (!teapot_aarch64_pac_fpac()) {
        /* Without FPAC the authentication returns a poisoned pointer and only
         * the rewriter's software return predicate rejects it; this host/QEMU
         * cannot execute that path end to end. */
        puts("PAC active without FPAC; poisoned authentication relies on the software predicate");
        return 0;
    }
    for (unsigned custom = 0; custom < 2; custom++) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            checkpoint_cnt = 0;
            struct sigaction action = {.sa_sigaction = forwarded_pac, .sa_flags = SA_SIGINFO};
            if (!custom) action = (struct sigaction){.sa_handler = SIG_DFL};
            sigemptyset(&action.sa_mask);
            assert(sigaction(SIGILL, &action, NULL) == 0);
            setup_signal_handler();
            teapot_aarch64_pac_activate();
            teapot_pac_bad_auth();
            _exit(1);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        if (custom)
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        else
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
    }
    checkpoint_cnt = 0;
    instruction_cnt = 17;
    test_memory = UINT64_C(0x123456789abcdef);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) dift_reg_tags[i] = (uint8_t)(i + 1);
    memory_history_t *saved_history = memory_history_top;
    uint32_t *saved_guards = guard_list_top;
    uint64_t saved_reason = simulation_statistics.rollback_reason[ROLLBACK_MALFORMED_INDIRECT_BR];
    uint64_t saved_rollbacks = teapot_pac_rollbacks;
    max_checkpoints = MAX_CHECKPOINTS;
    assert(checkpoint_pac_probe(stack_top, teapot_pac_bad_auth) == 42);
    assert(checkpoint_cnt == 0 && instruction_cnt == 17);
    assert(test_memory == UINT64_C(0x123456789abcdef));
    assert(memory_history_top == saved_history && guard_list_top == saved_guards);
    assert(!in_restore_memlog);
    for (size_t i = 0; i < DIFT_REG_TAGS_SIZE; i++) assert(dift_reg_tags[i] == i + 1);
    assert(simulation_statistics.rollback_reason[ROLLBACK_MALFORMED_INDIRECT_BR] == saved_reason + 1);
    assert(teapot_pac_rollbacks == saved_rollbacks + 1);
    puts("PAC activation, depth-zero forwarding and FPAC simulation rollback passed");
    return 0;
}
