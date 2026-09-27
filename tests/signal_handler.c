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
                uintptr_t sp = top - 2 * sizeof(uintptr_t);
                assert(context.uc_mcontext.gregs[REG_RSP] == sp);
                assert(*(uintptr_t *)sp ==
                       (uintptr_t)&restore_checkpoint_after_memlog);
                // RET enters the C continuation with the usual return-slot
                // offset, so its calls (including coverage) stay ABI-aligned.
                assert((sp + sizeof(uintptr_t)) % 16 == 8);
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
    const int signals[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE,
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
        SIGTRAP,
#endif
    };
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

static void check_default_refault(void) {
    const int signals[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE};
    const int codes[] = {ILL_ILLOPC, SEGV_MAPERR, BUS_ADRERR, FPE_INTDIV};
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
            ucontext_t context = {0};
            siginfo_t info = {.si_signo = signals[i], .si_code = codes[i]};
            info.si_addr = (void *)0x1234000;
            *context_pc(&context) = 0x400000;
            /* A precise fault must return the unchanged context to the
             * kernel, not queue a new SI_TKILL that replaces its siginfo. */
            signal_handler(signals[i], &info, &context);
            assert(*context_pc(&context) == 0x400000);
            assert(info.si_addr == (void *)0x1234000 && info.si_code == codes[i]);
            struct sigaction action;
            assert(sigaction(signals[i], NULL, &action) == 0);
            assert(action.sa_handler == SIG_DFL);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
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

static volatile sig_atomic_t application_calls;
static void usr1_handler(int sig) {
    assert(sig == SIGUSR1);
    ++application_calls;
}

static void check_unmanaged_signal(void) {
    struct sigaction action = {.sa_handler = usr1_handler, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    setup_signal_handler();
    struct sigaction actual;
    assert(sigaction(SIGUSR1, NULL, &actual) == 0);
    assert(actual.sa_handler == usr1_handler && (actual.sa_flags & SA_RESTART));
    assert(sigismember(&actual.sa_mask, SIGUSR2) == 1);
    raise(SIGUSR1);
    assert(application_calls == 1);

    /* Preserving an explicit handler must not special-case SIG_DFL either. */
    assert(signal(SIGUSR1, SIG_DFL) != SIG_ERR);
    setup_signal_handler();
    assert(sigaction(SIGUSR1, NULL, &actual) == 0);
    assert(actual.sa_handler == SIG_DFL);
}

static void replacement_handler(int sig) {
    assert(sig == SIGILL);
    ++application_calls;
}

static void info_handler(int sig, siginfo_t *info, void *context) {
    sigset_t mask;
    assert(sig == SIGILL && info->si_signo == SIGILL && context);
    assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGINT) == 1); /* interrupted mask */
    assert(sigismember(&mask, SIGUSR2) == 1); /* application sa_mask */
    assert(sigismember(&mask, SIGILL) == 0); /* SA_NODEFER */
    ++application_calls;
}

static void check_post_setup_registration(void) {
    signal(SIGILL, SIG_DFL);
    setup_signal_handler();
    assert(signal__teapot_wrapper__(SIGILL, replacement_handler) == SIG_DFL);
    assert(signal__teapot_wrapper__(SIGILL, SIG_IGN) == replacement_handler);
    assert(signal__teapot_wrapper__(SIGILL, replacement_handler) == SIG_IGN);
    struct sigaction actual, queried;
    assert(sigaction(SIGILL, NULL, &actual) == 0);
    assert(actual.sa_sigaction == signal_handler && (actual.sa_flags & SA_SIGINFO));
    assert(sigaction__teapot_wrapper__(SIGILL, NULL, &queried) == 0);
    assert(queried.sa_handler == replacement_handler);
    /* Repeat setup must not replace the saved handler with Teapot itself. */
    setup_signal_handler();
    raise(SIGILL);
    assert(application_calls == 1);

    sigset_t original, mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    assert(sigprocmask(SIG_SETMASK, &mask, &original) == 0);
    struct sigaction action = {
        .sa_sigaction = info_handler, .sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND
    };
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    assert(sigaction__teapot_wrapper__(SIGILL, &action, &queried) == 0);
    assert(queried.sa_handler == replacement_handler);
    raise(SIGILL);
    assert(application_calls == 2);
    assert(sigaction__teapot_wrapper__(SIGILL, NULL, &queried) == 0);
    assert(queried.sa_handler == SIG_DFL);
    assert(sigprocmask(SIG_SETMASK, &original, NULL) == 0);

    /* A handler installed after startup must never receive a simulated fault. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        assert(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);
        assert(signal__teapot_wrapper__(SIGILL, replacement_handler) == SIG_DFL);
        expect_redirect = true;
        checkpoint_cnt = 1;
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
        check_default_refault();
        check_forwarding_stack_budget();
    } else if (strcmp(argv[1], "redirect") == 0)
        check_signal_return();
    else if (strcmp(argv[1], "registration") == 0)
        check_post_setup_registration();
    else if (strcmp(argv[1], "unmanaged") == 0)
        check_unmanaged_signal();
    else
        return 1;
    return 0;
}
