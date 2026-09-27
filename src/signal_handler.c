#define _GNU_SOURCE
#include "signal_handler.h"
#include "checkpoint.h"

#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
/* Weak so the standalone signal tests and ordinary runtime users need not
 * link/enable the experimental backend. Rewritten BTI inputs require its
 * strong initialization symbol explicitly. */
extern bool teapot_aarch64_bti_signal(int, siginfo_t *, void *) __attribute__((weak));
#endif

// Older libc headers predate this Linux auxiliary-vector entry.
#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif

struct saved_signal_action {
    int sig;
    struct sigaction action;
    bool valid;
};

static struct saved_signal_action saved_signal_actions[] = {
    { .sig = SIGSEGV },
    { .sig = SIGILL },
    { .sig = SIGFPE },
    { .sig = SIGBUS },
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    /* BRK has exception priority over BTI at a guarded indirect target. */
    { .sig = SIGTRAP },
#endif
};

static struct saved_signal_action *saved_action_for_signal(int sig) {
    for (size_t i = 0; i < sizeof(saved_signal_actions) / sizeof(saved_signal_actions[0]); i++) {
        if (saved_signal_actions[i].sig == sig) {
            return &saved_signal_actions[i];
        }
    }
    return NULL;
}

static bool signal_can_refault(int sig, const siginfo_t *info) {
    if (!info || info->si_code <= 0)
        return false;
    /* These positive-code reports are asynchronous: returning need not
     * execute the instruction that caused them again. */
#ifdef SEGV_MTEAERR
    if (sig == SIGSEGV && info->si_code == SEGV_MTEAERR)
        return false;
#endif
#ifdef BUS_MCEERR_AO
    if (sig == SIGBUS && info->si_code == BUS_MCEERR_AO)
        return false;
#endif
    return true;
}

static void invoke_saved_signal_action(int sig, siginfo_t *info, void *ucontext) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    const struct sigaction default_action = { .sa_handler = SIG_DFL };
    // A handler may replace itself. Snapshot its disposition before calling it.
    const struct sigaction snapshot = saved && saved->valid ? saved->action : default_action;
    const struct sigaction *action = &snapshot;
    if (action->sa_handler == SIG_IGN) {
        return;
    } else if (action->sa_handler == SIG_DFL) {
        sigaction(sig, action, NULL);
        // A precise hardware fault repeats at the original instruction after
        // sigreturn, preserving its code and address for the core/tracer.
        // Re-raise only signals that would not naturally occur again.
        if (!signal_can_refault(sig, info))
            raise(sig);
    } else {
        if ((action->sa_flags & SA_RESETHAND) && saved)
            saved->action = default_action;
        // Teapot blocks all signals while editing the fault context, but an
        // application handler must observe its own mask and NODEFER contract.
        sigset_t mask = ((ucontext_t *)ucontext)->uc_sigmask, previous;
        for (int number = 1; number < NSIG; ++number)
            if (sigismember(&action->sa_mask, number) == 1)
                sigaddset(&mask, number);
        if (!(action->sa_flags & SA_NODEFER))
            sigaddset(&mask, sig);
        sigprocmask(SIG_SETMASK, &mask, &previous);
        if (action->sa_flags & SA_SIGINFO)
            action->sa_sigaction(sig, info, ucontext);
        else
            action->sa_handler(sig);
        sigprocmask(SIG_SETMASK, &previous, NULL);
    }
}

static uintptr_t *signal_program_counter(void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
#if defined(__x86_64__)
    return (uintptr_t *)&uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t *)&uc->uc_mcontext.pc;
#elif defined(__riscv) && __riscv_xlen == 64
#ifndef REG_PC
#define REG_PC 0
#endif
    return (uintptr_t *)&uc->uc_mcontext.__gregs[REG_PC];
#else
#error "Unsupported libcheckpoint signal architecture"
#endif
}

static void restart_restore_checkpoint_memlog(void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t stack_top = (uintptr_t)scratchpad + SCRATCHPAD_SIZE;
    uintptr_t continuation = (uintptr_t)&restore_checkpoint_after_memlog;

#if defined(__x86_64__)
    // restore_checkpoint_memlog returns into an ordinary C continuation.
    // Leave RSP at 8 mod 16 after that RET, not at the aligned stack top.
    uintptr_t sp = stack_top - 2 * sizeof(uintptr_t);
    *(uintptr_t *)sp = continuation;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)sp;
#elif defined(__aarch64__)
    uc->uc_mcontext.sp = stack_top;
    uc->uc_mcontext.regs[30] = continuation;
#elif defined(__riscv) && __riscv_xlen == 64
    uc->uc_mcontext.__gregs[1] = continuation;
    uc->uc_mcontext.__gregs[2] = stack_top;
#else
#error "Unsupported libcheckpoint signal architecture"
#endif
    *signal_program_counter(ucontext) = (uintptr_t)&restore_checkpoint_memlog;
}

void signal_handler(int sig, siginfo_t *info, void *ucontext) {
    uintptr_t *pc = signal_program_counter(ucontext);

    if (in_restore_memlog) {
        restart_restore_checkpoint_memlog(ucontext);
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    } else if (teapot_aarch64_bti_signal && teapot_aarch64_bti_signal(sig, info, ucontext)) {
        return;
#endif
    } else if (checkpoint_cnt != 0) {
        *pc = (uintptr_t)&restore_checkpoint_SIGSEGV;
    } else {
        /* The interrupted code may hold stdio/locale locks. Keep forwarding
         * diagnostics async-signal-safe and preserve the application's errno. */
        static const char message[] = "Teapot: forwarding signal outside simulation\n";
        int interrupted_errno = errno;
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        errno = interrupted_errno;
        invoke_saved_signal_action(sig, info, ucontext);
    }
}

static void install_signal_handler(int sig, const struct sigaction *action) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    struct sigaction previous;
    if (sigaction(sig, action, &previous) != 0) {
        perror("sigaction");
        abort();
    }
    // Re-enabling the runtime must not chain its handler back to itself.
    if (saved && !((previous.sa_flags & SA_SIGINFO) &&
                   previous.sa_sigaction == signal_handler)) {
        saved->action = previous;
        saved->valid = true;
    }
}

int sigaction__teapot_wrapper__(int sig, const struct sigaction *action,
                                struct sigaction *old_action) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    if (!saved || !saved->valid)
        return sigaction(sig, action, old_action);

    // Only calls from rewritten code use this entry. Runtime/libc/ASan calls
    // still reach libc directly, avoiding interposition recursion at startup.
    sigset_t all, previous_mask;
    sigfillset(&all);
    if (sigprocmask(SIG_BLOCK, &all, &previous_mask) != 0)
        return -1;
    const struct sigaction previous_action = saved->action;
    int result = 0;
    if (action) {
        const struct sigaction next_action = *action;
        struct sigaction runtime_action = {
            .sa_sigaction = signal_handler,
            .sa_flags = SA_ONSTACK | SA_SIGINFO | (action->sa_flags & SA_RESTART)
        };
        sigfillset(&runtime_action.sa_mask);
        result = sigaction(sig, &runtime_action, NULL);
        if (result == 0)
            saved->action = next_action;
    }
    if (result == 0 && old_action)
        *old_action = previous_action;
    const int saved_errno = errno;
    sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    errno = saved_errno;
    return result;
}

teapot_signal_function signal__teapot_wrapper__(int sig, teapot_signal_function handler) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    if (!saved || !saved->valid)
        return signal(sig, handler);
    if (handler == SIG_ERR) {
        errno = EINVAL;
        return SIG_ERR;
    }
    // Linux/glibc's default signal() interface uses BSD semantics.
    struct sigaction action = { .sa_handler = handler, .sa_flags = SA_RESTART }, previous;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, sig);
    if (sigaction__teapot_wrapper__(sig, &action, &previous) != 0)
        return SIG_ERR;
    return previous.sa_handler;
}

void setup_signal_handler() {
    // The kernel's frame (including extended CPU state) and the handler's
    // stack are separate budgets. SIGSTKSZ alone can leave too little room
    // for forwarding an application's handler on older libc/newer CPUs.
    // Keep this independent of application/scratch stacks and guard both ends.
    static stack_t ss;
    if (ss.ss_sp == NULL) {
        const size_t handler_budget = 64 * 1024;
        size_t frame_size = getauxval(AT_MINSIGSTKSZ);
        if (frame_size < (size_t)SIGSTKSZ)
            frame_size = (size_t)SIGSTKSZ;
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0 ||
                frame_size > SIZE_MAX - handler_budget - 3 * (size_t)page_size)
            abort();
        size_t size = ((frame_size + handler_budget + page_size - 1) /
                       page_size) * page_size;
        void *mapping = mmap(NULL, size + 2 * page_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            perror("mmap signal stack");
            abort();
        }
        void *stack = (char *)mapping + page_size;
        if (mprotect(stack, size, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect signal stack");
            munmap(mapping, size + 2 * page_size);
            abort();
        }
        ss.ss_sp = stack;
        ss.ss_size = size;
    }
    struct sigaction sa = {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_ONSTACK | SA_SIGINFO
    };
    if (sigaltstack(&ss, NULL) != 0) {
        perror("sigaltstack");
        abort();
    }
    sigfillset(&sa.sa_mask);
    install_signal_handler(SIGSEGV, &sa);
    install_signal_handler(SIGILL, &sa);
    install_signal_handler(SIGFPE, &sa);
    install_signal_handler(SIGBUS, &sa);
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    install_signal_handler(SIGTRAP, &sa);
#endif

}
