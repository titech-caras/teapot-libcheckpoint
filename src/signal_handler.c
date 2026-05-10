#include "signal_handler.h"
#include "checkpoint.h"
#include "report_gadget.h"

#define __USE_GNU
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#if defined(__riscv) && __riscv_xlen == 64
static bool looks_like_userspace_pc(uintptr_t value) {
    /*
     * User-mode qemu and native Linux place shared libraries outside the main
     * executable.  Treat non-zero positive addresses as plausible PCs so an
     * old qemu signal frame can still recover a shared-library fault address.
     */
    return value != 0 && value < (uintptr_t)1 << 63;
}
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
    if (!saved || !saved->valid) {
        signal(sig, SIG_DFL);
        raise(sig);
        abort();
    }

    struct sigaction *action = &saved->action;
    if (action->sa_flags & SA_SIGINFO) {
        if (action->sa_sigaction) {
            action->sa_sigaction(sig, info, ucontext);
            return;
        }
    } else if (action->sa_handler == SIG_IGN) {
        return;
    } else if (action->sa_handler && action->sa_handler != SIG_DFL) {
        action->sa_handler(sig);
        return;
    }

    sigaction(sig, action, NULL);
    raise(sig);
    abort();
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
    uintptr_t *pc = (uintptr_t *)&uc->uc_mcontext.__gregs[REG_PC];
    /*
     * Native RISC-V Linux and current user-mode emulators restore the PC from
     * uc_mcontext.__gregs[REG_PC].  The older qemu-riscv64 used by the eval
     * image restores it from an older raw signal-frame slot instead, while the
     * glibc field contains a small non-address value.  Keep the ABI path as the
     * default and use the compatibility slot only when the ABI field is clearly
     * not a userspace PC.
     */
    if (!looks_like_userspace_pc(*pc)) {
        uintptr_t *qemu_compat_pc = &((uintptr_t *)ucontext)[5];
        if (looks_like_userspace_pc(*qemu_compat_pc)) {
            return qemu_compat_pc;
        }
    }
    return pc;
#else
#error "Unsupported libcheckpoint signal architecture"
#endif
}

static void signal_set_program_counter(void *ucontext, uintptr_t target) {
    uintptr_t *pc = signal_program_counter(ucontext);
#if defined(__riscv) && __riscv_xlen == 64
    uintptr_t *qemu_compat_pc = &((uintptr_t *)ucontext)[5];
    bool update_qemu_compat_pc = qemu_compat_pc != pc && looks_like_userspace_pc(*qemu_compat_pc);
#endif
    *pc = target;
#if defined(__riscv) && __riscv_xlen == 64
    if (update_qemu_compat_pc) {
        *qemu_compat_pc = target;
    }
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
    signal_set_program_counter(ucontext, (uintptr_t)&restore_checkpoint_memlog);
}

void signal_handler(int sig, siginfo_t *info, void *ucontext) {
    uintptr_t *pc = signal_program_counter(ucontext);

    if (checkpoint_cnt != 0) {
        //report_gadget_SIGSEGV((uint64_t) *pc, (uint64_t) info->si_addr);
        signal_set_program_counter(ucontext, (uintptr_t)&restore_checkpoint_SIGSEGV);
    } else if (in_restore_memlog) {
        restart_restore_checkpoint_memlog(ucontext);
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
    static char signal_stack[SIGSTKSZ]; // so that SIGSEGV doesn't overwrite stack contents in speculation
    static stack_t ss = {
        .ss_size = SIGSTKSZ,
        .ss_sp = signal_stack,
    };
    struct sigaction sa = {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_ONSTACK | SA_SIGINFO
    };
    sigaltstack(&ss, 0);
    sigfillset(&sa.sa_mask);
    install_signal_handler(SIGSEGV, &sa);
    install_signal_handler(SIGILL, &sa);
    install_signal_handler(SIGFPE, &sa);
    install_signal_handler(SIGBUS, &sa);

    signal(SIGUSR1, SIG_IGN);
}
