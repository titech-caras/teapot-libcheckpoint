#define _GNU_SOURCE
#include "signal_handler.h"
#include "checkpoint.h"

#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

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
};

static struct saved_signal_action *saved_action_for_signal(int sig) {
    for (size_t i = 0; i < sizeof(saved_signal_actions) / sizeof(saved_signal_actions[0]); i++) {
        if (saved_signal_actions[i].sig == sig) {
            return &saved_signal_actions[i];
        }
    }
    return NULL;
}

static void invoke_saved_signal_action(int sig, siginfo_t *info, void *ucontext) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    const struct sigaction default_action = { .sa_handler = SIG_DFL };
    const struct sigaction *action = saved && saved->valid ?
        &saved->action : &default_action;
    if (action->sa_handler == SIG_IGN) {
        return;
    } else if (action->sa_handler == SIG_DFL) {
        sigaction(sig, action, NULL);
        // Delivery resumes with the original disposition when this handler
        // returns and the kernel restores the caller's signal mask.
        raise(sig);
    } else if (action->sa_flags & SA_SIGINFO) {
        action->sa_sigaction(sig, info, ucontext);
    } else {
        action->sa_handler(sig);
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
    uintptr_t sp = stack_top - sizeof(uintptr_t);
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
    } else if (checkpoint_cnt != 0) {
        *pc = (uintptr_t)&restore_checkpoint_SIGSEGV;
    } else {
        fprintf(stderr, "Signal caught outside simulation, forwarding: %s at pc=0x%lx addr=0x%lx\n",
                strsignal(sig), (unsigned long)*pc, (unsigned long)info->si_addr);
        invoke_saved_signal_action(sig, info, ucontext);
    }
}

static void install_signal_handler(int sig, const struct sigaction *action) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    sigaction(sig, action, saved ? &saved->action : NULL);
    if (saved) {
        saved->valid = true;
    }
}

void setup_signal_handler() {
    // The kernel's frame (including extended CPU state) and the handler's
    // stack are separate budgets. SIGSTKSZ alone can leave too little room
    // for libc's forwarding diagnostic on older libc/newer CPU combinations.
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

    signal(SIGUSR1, SIG_IGN);
}
