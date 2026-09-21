#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include "checkpoint.h"
#include "signal_handler.h"

scratchpad_t scratchpad;
uint64_t checkpoint_cnt;
volatile uint64_t in_restore_memlog;
static bool expect_redirect;

void restore_checkpoint_SIGSEGV(void) {
    sigset_t mask;
    if (expect_redirect && sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
            sigismember(&mask, SIGINT) == 1 && sigismember(&mask, SIGILL) == 0)
        _exit(0);
    abort();
}
void restore_checkpoint_after_memlog(void) { abort(); }
void restore_checkpoint_memlog(void) { abort(); }

void signal_handler(int sig, siginfo_t *info, void *ucontext);

static uintptr_t *context_pc(ucontext_t *context) {
#if defined(__x86_64__)
    return (uintptr_t *)&context->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t *)&context->uc_mcontext.pc;
#else
    return (uintptr_t *)&context->uc_mcontext.__gregs[REG_PC];
#endif
}

static void check_contexts(void) {
    for (unsigned depth = 0; depth <= 2; depth++) {
        for (unsigned replay = 0; replay <= 1; replay++) {
            if (!depth && !replay)
                continue;
            ucontext_t context = {0};
            siginfo_t info = {0};
            sigemptyset(&context.uc_sigmask);
            sigaddset(&context.uc_sigmask, SIGINT);
            sigset_t mask = context.uc_sigmask;
            *context_pc(&context) = 0x400000;
            checkpoint_cnt = depth;
            in_restore_memlog = replay;
            signal_handler(SIGSEGV, &info, &context);
            assert(memcmp(&context.uc_sigmask, &mask, sizeof(mask)) == 0);
            assert(checkpoint_cnt == depth);
            assert(*context_pc(&context) == (replay ?
                   (uintptr_t)&restore_checkpoint_memlog :
                   (uintptr_t)&restore_checkpoint_SIGSEGV));
            if (replay) {
                uintptr_t top = (uintptr_t)scratchpad + SCRATCHPAD_SIZE;
#if defined(__x86_64__)
                assert(context.uc_mcontext.gregs[REG_RSP] == top - sizeof(uintptr_t));
                assert(*(uintptr_t *)(top - sizeof(uintptr_t)) ==
                       (uintptr_t)&restore_checkpoint_after_memlog);
#elif defined(__aarch64__)
                assert(context.uc_mcontext.sp == top);
                assert(context.uc_mcontext.regs[30] ==
                       (uintptr_t)&restore_checkpoint_after_memlog);
#else
                assert(context.uc_mcontext.__gregs[2] == top);
                assert(context.uc_mcontext.__gregs[1] ==
                       (uintptr_t)&restore_checkpoint_after_memlog);
#endif
            }
        }
    }
}

static void check_default_forwarding(void) {
    const int signals[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct rlimit limit = {0, 0};
            assert(setrlimit(RLIMIT_CORE, &limit) == 0);
            checkpoint_cnt = 0;
            in_restore_memlog = false;
            signal(signals[i], SIG_DFL);
            setup_signal_handler();
            raise(signals[i]);
            _exit(1);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status));
        assert(WTERMSIG(status) == signals[i]);
    }
}

static void stack_using_saved_handler(int sig) {
    volatile unsigned char work[32 * 1024];
    for (size_t i = 0; i < sizeof(work); i++)
        work[i] = (unsigned char)i;
    for (size_t i = 0; i < sizeof(work); i++)
        if (work[i] != (unsigned char)i)
            _exit(2);
    _exit(sig == SIGILL ? 0 : 3);
}

static void check_forwarding_stack_budget(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        checkpoint_cnt = 0;
        in_restore_memlog = false;
        signal(SIGILL, stack_using_saved_handler);
        setup_signal_handler();
        stack_t stack;
        assert(sigaltstack(NULL, &stack) == 0);
        assert(!(stack.ss_flags & SS_DISABLE));
        assert(stack.ss_size >= 64 * 1024 + (size_t)SIGSTKSZ);
        raise(SIGILL);
        _exit(1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void check_signal_return(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        assert(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);
        expect_redirect = true;
        checkpoint_cnt = 1;
        setup_signal_handler();
        raise(SIGILL);
        _exit(1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "context") == 0)
        check_contexts();
    else if (strcmp(argv[1], "forward") == 0) {
        check_default_forwarding();
        check_forwarding_stack_budget();
    } else if (strcmp(argv[1], "redirect") == 0)
        check_signal_return();
    else
        return 1;
    return 0;
}
