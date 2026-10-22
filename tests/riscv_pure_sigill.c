/* White-box C1 coverage, including replay at depth zero. The separately
 * registered entry tests also fault after actual make_checkpoint_riscv64. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "checkpoint.h"
#include "signal_handler.h"
#include "riscv_pure_signal_verify.h"

#ifndef TEAPOT_RISCV_LP64_PURE
#error "This negative fixture is PURE-only"
#endif

scratchpad_t scratchpad;
uint64_t checkpoint_cnt;
volatile uint64_t in_restore_memlog;
static volatile uint64_t *route_calls;
void signal_handler(int, siginfo_t *, void *);
void restore_checkpoint_SIGSEGV(void) { ++route_calls[0]; _exit(90); }
void restore_checkpoint_after_memlog(void) { ++route_calls[1]; _exit(91); }
void restore_checkpoint_memlog(void) { ++route_calls[2]; _exit(92); }
int __wrap_fprintf(FILE *stream, const char *format, ...) { (void)stream; (void)format; _exit(93); }
char *__wrap_strsignal(int sig) { (void)sig; _exit(94); }

int main(void) {
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    route_calls = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(route_calls != MAP_FAILED);
    pure_verified_kernel_events = route_calls + 3;
    unsigned cases = 0;
    const int codes[] = {ILL_ILLOPC, ILL_ILLOPN, ILL_ILLADR, ILL_ILLTRP, ILL_PRVOPC};
    for (unsigned depth = 0; depth <= 2; ++depth)
    for (unsigned replay = 0; replay <= 1; ++replay) {
        if (!depth && !replay)
            continue;
        /* Synthetic synchronous codes, plus four real kernel-delivered
         * instructions under the independently qualified no-F/D runner. */
        for (unsigned fixture = 0; fixture < sizeof(codes) / sizeof(codes[0]) + 4; ++fixture) {
            int output[2]; assert(pipe(output) == 0);
            pid_t child = fork(); assert(child >= 0);
            if (!child) {
                close(output[0]);
                assert(dup2(output[1], STDERR_FILENO) >= 0); close(output[1]);
                checkpoint_cnt = depth; in_restore_memlog = replay;
                if (fixture < sizeof(codes) / sizeof(codes[0])) {
                    ucontext_t context = {0};
                    siginfo_t info = {.si_signo = SIGILL, .si_code = codes[fixture]};
                    context.uc_mcontext.__gregs[REG_PC] = 0x400000;
                    signal_handler(SIGILL, &info, &context);
                } else {
                    setup_signal_handler();
                    pure_sigill_instruction(fixture - sizeof(codes) / sizeof(codes[0]) + 1);
                }
                _exit(95);  /* Fatal routing must never return. */
            }
            close(output[1]);
            char diagnostic[256] = {0}; size_t length = 0; ssize_t count;
            while ((count = read(output[0], diagnostic + length, sizeof(diagnostic) - 1 - length)) > 0)
                length += (size_t)count;
            assert(count == 0); close(output[0]);
            int status; assert(waitpid(child, &status, 0) == child);
            assert(WIFEXITED(status) && WEXITSTATUS(status) == LIBCHECKPOINT_RISCV_PURE_SIGILL_EXIT_STATUS);
            assert(strcmp(diagnostic, LIBCHECKPOINT_RISCV_PURE_SIGILL_DIAGNOSTIC) == 0);
            assert(route_calls[0] == 0 && route_calls[1] == 0 && route_calls[2] == 0);
            ++cases;
        }
    }
    assert(route_calls[3] == 20);
    printf("{\"fatal_sigill_cases\":%u,\"exact_pc_kernel_sigill_events\":20,"
           "\"rollback_calls\":0,\"memlog_restart_calls\":0}\n", cases);
    assert(munmap((void *)route_calls, 4096) == 0);
    return 0;
}
