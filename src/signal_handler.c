#define _GNU_SOURCE
#include "signal_handler.h"
#include "checkpoint.h"
#include "fault_sites.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
/* Weak so the standalone signal tests and ordinary runtime users need not
 * link/enable the experimental backend. Rewritten BTI inputs require its
 * strong initialization symbol explicitly. */
extern bool teapot_aarch64_bti_signal(int, siginfo_t *, void *) __attribute__((weak));
extern bool teapot_aarch64_pac_signal(int, siginfo_t *, void *) __attribute__((weak));
#endif

// Older libc headers predate this Linux auxiliary-vector entry.
#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif

extern uintptr_t checkpoint_target_metadata[];

struct saved_signal_action {
    struct sigaction action;
    bool valid;
};

/* Read by the handler during simulation: protected like the runtime state. */
static struct saved_signal_action saved_signal_actions[NSIG] LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(saved_signal_actions);

/* saved_signal_actions is poisoned like the rest of the protected state, and a
 * struct assignment may become a call to memcpy (unoptimized builds, or copies
 * above a compiler's inline limit), which ASan intercepts and checks. Copy
 * whole words through volatile accesses instead, as the checkpoint restore does
 * for the protected tag storage. */
static void copy_signal_action(struct sigaction *dst, const struct sigaction *src) {
    typedef uint64_t action_word_t __attribute__((may_alias));
    _Static_assert(sizeof(struct sigaction) % sizeof(action_word_t) == 0, "whole sigaction words");
    _Static_assert(_Alignof(struct sigaction) % _Alignof(action_word_t) == 0, "aligned sigaction words");
    volatile action_word_t *to = (volatile action_word_t *)dst;
    const volatile action_word_t *from = (const volatile action_word_t *)src;
    for (size_t i = 0; i < sizeof(struct sigaction) / sizeof(action_word_t); i++)
        to[i] = from[i];
}

static struct saved_signal_action *saved_action_for_signal(int sig) {
    return sig > 0 && sig < NSIG ? &saved_signal_actions[sig] : NULL;
}

static bool signal_can_refault(int sig, const siginfo_t *info) {
    switch (sig) {
    case SIGSEGV: case SIGILL: case SIGFPE: case SIGBUS:
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    case SIGTRAP:
#endif
        break;
    default:
        return false;
    }
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
    struct sigaction snapshot;
    copy_signal_action(&snapshot, saved && saved->valid ? &saved->action : &default_action);
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
            copy_signal_action(&saved->action, &default_action);
        // Teapot blocks all signals while editing the fault context, but an
        // application handler must observe its own mask and NODEFER contract.
        sigset_t mask = ((ucontext_t *)ucontext)->uc_sigmask, previous;
        for (int number = 1; number < NSIG; ++number)
            if (sigismember(&action->sa_mask, number) == 1)
                sigaddset(&mask, number);
        if (!(action->sa_flags & SA_NODEFER))
            sigaddset(&mask, sig);
        uintptr_t landing_register = checkpoint_target_metadata[CHECKPOINT_TARGET_SCRATCH_REG_ADDR / 8];
        sigprocmask(SIG_SETMASK, &mask, &previous);
        if (action->sa_flags & SA_SIGINFO)
            action->sa_sigaction(sig, info, ucontext);
        else
            action->sa_handler(sig);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        checkpoint_target_metadata[CHECKPOINT_TARGET_SCRATCH_REG_ADDR / 8] = landing_register;
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

/* Linux's kernel sigset has 64 bits on each supported ISA. Only bits added to
 * the interrupted mask belong here; application-blocked signals stay blocked. */
_Static_assert(NSIG == 65, "Linux kernel signal mask");
uint64_t teapot_pending_signals LIBCHECKPOINT_PROTECTED_SECTION;
uintptr_t teapot_signal_resume_target LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_pending_signals);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_signal_resume_target);
extern char __start_teapot_checkpoint[], __stop_teapot_checkpoint[];
extern char __start_teapot_restore[], __stop_teapot_restore[];
extern char teapot_restore_registers_end[], teapot_checkpoint_skip_jump[];
extern char teapot_signal_resume_mask_load[], teapot_signal_resume_mask_ready[];
extern char teapot_signal_resume_unblocked[];
extern void teapot_signal_resume(void);

static bool pc_in(uintptr_t pc, const void *start, const void *end) {
    return pc >= (uintptr_t)start && pc < (uintptr_t)end;
}

static uintptr_t signal_stack_pointer(void *context) {
    ucontext_t *uc = context;
#if defined(__x86_64__)
    return uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
    return uc->uc_mcontext.sp;
#else
    return uc->uc_mcontext.__gregs[2];
#endif
}

bool teapot_signal_runtime_busy(void *context) {
    uintptr_t pc = *signal_program_counter(context);
    uintptr_t sp = signal_stack_pointer(context);
    return checkpoint_cnt != 0 || in_restore_memlog ||
        pc_in(pc, __start_teapot_checkpoint, __stop_teapot_checkpoint) ||
        pc_in(pc, __start_teapot_restore, __stop_teapot_restore) ||
        pc_in(pc, teapot_signal_resume, teapot_signal_resume_unblocked) ||
        /* Restore callbacks (coverage, fault publishing) also borrow this
         * stack, even when their PC is outside the runtime's own section. */
        (sp >= (uintptr_t)scratchpad && sp < (uintptr_t)scratchpad + SCRATCHPAD_SIZE);
}

LIBCHECKPOINT_RESTORE_PATH void teapot_signal_prepare_resume(uint64_t *target) {
    if (*target != (uintptr_t)teapot_signal_resume) {
        teapot_signal_resume_target = *target;
        *target = (uintptr_t)teapot_signal_resume;
    }
}

static void defer_signal(int sig, siginfo_t *info, void *context) {
    int interrupted_errno = errno;
    ucontext_t *uc = context;
    uintptr_t *pc = signal_program_counter(context);
    uint64_t bit = UINT64_C(1) << (sig - 1);
    if (sigismember(&uc->uc_sigmask, sig) != 1) {
        sigaddset(&uc->uc_sigmask, sig);
        if (pc_in(*pc, teapot_signal_resume_mask_ready, teapot_signal_resume_unblocked)) {
            /* The stub has transferred ownership to its private stack frame.
             * A second signal before the syscall must join that exact mask. */
            *(uint64_t *)signal_stack_pointer(context) |= bit;
        } else {
            teapot_pending_signals |= bit;
            if (pc_in(*pc, teapot_signal_resume_mask_load, teapot_signal_resume_mask_ready))
                *pc = (uintptr_t)teapot_signal_resume_mask_load;
        }
    }
    int result = syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), sig, info);
    assert(result == 0);

    if (checkpoint_cnt == 0) {
        if (pc_in(*pc, __start_teapot_checkpoint, __stop_teapot_checkpoint)) {
            /* An entry may skip speculation altogether. Its ordinary return
             * still has to release the signal; a selected entry saves this
             * same return target in checkpoint_metadata[0]. */
            teapot_signal_prepare_resume(&checkpoint_target_metadata[CHECKPOINT_TARGET_RETURN_ADDR / 8]);
#if defined(__aarch64__)
            if (*pc == (uintptr_t)teapot_checkpoint_skip_jump)
                uc->uc_mcontext.regs[16] = (uintptr_t)teapot_signal_resume;
#elif defined(__riscv)
            if (*pc == (uintptr_t)teapot_checkpoint_skip_jump)
                uc->uc_mcontext.__gregs[5] = (uintptr_t)teapot_signal_resume;
#endif
        } else if (pc_in(*pc, __start_teapot_restore, __stop_teapot_restore)) {
            teapot_signal_prepare_resume(&checkpoint_metadata[0].return_address);
            /* A signal can land after C's pending check, or after assembly
             * loaded the continuation. Replay only the register loads; they
             * use the still-intact checkpoint and need no stack frame. */
            if (pc_in(*pc, restore_checkpoint_registers, teapot_restore_registers_end))
                *pc = (uintptr_t)restore_checkpoint_registers;
        }
    }
    errno = interrupted_errno;
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
    /* A signal number alone does not identify a speculative fault. kill(),
     * raise() and sigqueue() deliver application notifications with nonpositive
     * codes, even when they interrupt a checkpoint or memory-log replay. */
    const bool kernel_fault = signal_can_refault(sig, info);
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    bool has_handler = saved && saved->valid && saved->action.sa_handler != SIG_DFL &&
                       saved->action.sa_handler != SIG_IGN;
    if (!kernel_fault && has_handler && teapot_signal_runtime_busy(ucontext)) {
        defer_signal(sig, info, ucontext);
        return;
    }

#ifdef TEAPOT_RISCV_LP64_PURE
    /* C1: a synchronous illegal instruction violates the reviewed PURE proof.
     * Refuse before memory-log recovery as well as checkpoint rollback, so a
     * decode/emission gap cannot be converted into successful soft recovery.
     * No stdio, allocation, fault training or application callback is safe or
     * appropriate here. write and _exit are async-signal-safe. */
    if (sig == SIGILL && kernel_fault && (checkpoint_cnt != 0 || in_restore_memlog != 0)) {
        static const char message[] = LIBCHECKPOINT_RISCV_PURE_SIGILL_DIAGNOSTIC;
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(LIBCHECKPOINT_RISCV_PURE_SIGILL_EXIT_STATUS);
    }
#endif

#if defined(ENABLE_FAULT_TRAINING) && FAULT_RISC_NATIVE
    /* Synchronous v4 copies are transient-only. Do not inspect, print or
     * forward their changed bootstrap register, including impossible depth
     * zero/replay contexts. Asynchronous reports (including positive-code MTE
     * or machine-check notifications) and BTI/PAC/non-memory exceptions keep
     * their baseline routing below. */
    if ((sig == SIGSEGV || sig == SIGBUS) && signal_can_refault(sig, info) &&
            teapot_fault_risc_copied_kernel(*pc, checkpoint_cnt != 0, in_restore_memlog != 0)) {
        teapot_fault_train(sig, info, *pc, true, false);
        *pc = (uintptr_t)&restore_checkpoint_SIGSEGV;
        return;
    }
#endif

    if (kernel_fault && in_restore_memlog) {
        restart_restore_checkpoint_memlog(ucontext);
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    } else if (kernel_fault && teapot_aarch64_pac_signal &&
               teapot_aarch64_pac_signal(sig, info, ucontext)) {
        return;
    } else if (kernel_fault && teapot_aarch64_bti_signal &&
               teapot_aarch64_bti_signal(sig, info, ucontext)) {
        return;
#endif
    } else if (kernel_fault && checkpoint_cnt != 0) {
        /* Metadata/training only: fault classification and rollback are
         * unchanged. PAC/BTI handlers above have first refusal. */
#ifdef ENABLE_FAULT_TRAINING
        teapot_fault_train(sig, info, *pc, true, false);
#endif
        *pc = (uintptr_t)&restore_checkpoint_SIGSEGV;
    } else {
        /* Keep the diagnostic for the original fault signal numbers, including
         * kill/raise notifications. Other application signals forward silently.
         * The interrupted code may hold stdio/locale locks; preserve errno. */
        static const char message[] = "Teapot: forwarding application signal\n";
        int interrupted_errno = errno;
        if (sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
                || sig == SIGTRAP
#endif
        )
            (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        errno = interrupted_errno;
        if (has_handler)
            assert(!teapot_signal_runtime_busy(ucontext));
        invoke_saved_signal_action(sig, info, ucontext);
    }
}

static void install_signal_handler(int sig, const struct sigaction *action) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    struct sigaction previous;
    if (sigaction(sig, NULL, &previous) != 0) {
        perror("sigaction");
        abort();
    }
    // Re-enabling the runtime must not chain its handler back to itself.
    if (!((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction == signal_handler)) {
        copy_signal_action(&saved->action, &previous);
        saved->valid = true;
    }
    struct sigaction runtime_action = *action;
    runtime_action.sa_flags |= saved->action.sa_flags & (SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT);
    if (sigaction(sig, &runtime_action, NULL) != 0) {
        perror("sigaction");
        abort();
    }
}

int sigaction__teapot_wrapper__(int sig, const struct sigaction *action,
                                struct sigaction *old_action) {
    struct saved_signal_action *saved = saved_action_for_signal(sig);
    if (!saved)
        return sigaction(sig, action, old_action);

    // Only rewritten calls use this entry; runtime/libc/ASan still call libc.
    sigset_t all, previous_mask;
    sigfillset(&all);
    if (sigprocmask(SIG_BLOCK, &all, &previous_mask) != 0)
        return -1;
    struct sigaction previous_action;
    int result = sigaction(sig, NULL, &previous_action);
    if (result == 0 && (previous_action.sa_flags & SA_SIGINFO) &&
            previous_action.sa_sigaction == signal_handler && saved->valid)
        copy_signal_action(&previous_action, &saved->action);
    if (result == 0 && action) {
        const struct sigaction next_action = *action;
        struct sigaction runtime_action = {
            .sa_sigaction = signal_handler,
            .sa_flags = SA_ONSTACK | SA_SIGINFO |
                (action->sa_flags & (SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT))
        };
        sigfillset(&runtime_action.sa_mask);
        result = sigaction(sig, action->sa_handler == SIG_DFL || action->sa_handler == SIG_IGN
                               ? &next_action : &runtime_action, NULL);
        if (result == 0) {
            copy_signal_action(&saved->action, &next_action);
            saved->valid = true;
        }
    }
    if (result == 0 && old_action)
        *old_action = previous_action;
    const int saved_errno = errno;
    sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    errno = saved_errno;
    return result;
}

teapot_signal_function signal__teapot_wrapper__(int sig, teapot_signal_function handler) {
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

/* glibc hides this obsolete alias declaration with _GNU_SOURCE/XOPEN2K8,
 * although its exported entry point remains available for existing programs. */
extern teapot_signal_function bsd_signal(int, teapot_signal_function);

static teapot_signal_function install_sysv_signal(int sig, teapot_signal_function handler) {
    if (handler == SIG_ERR || sig < 1 || sig >= NSIG) {
        errno = EINVAL;
        return SIG_ERR;
    }
    struct sigaction action = {
        .sa_handler = handler, .sa_flags = SA_RESETHAND | SA_NODEFER
    }, previous;
#ifdef SA_INTERRUPT
    /* Obsolete on Linux, but present in glibc's recorded SysV action. */
    action.sa_flags |= SA_INTERRUPT;
#endif
    sigemptyset(&action.sa_mask);
    if (sigaction__teapot_wrapper__(sig, &action, &previous) != 0)
        return SIG_ERR;
    return previous.sa_handler;
}

teapot_signal_function __sysv_signal__teapot_wrapper__(int sig, teapot_signal_function handler) {
    return install_sysv_signal(sig, handler);
}

teapot_signal_function sysv_signal__teapot_wrapper__(int sig, teapot_signal_function handler) {
    return install_sysv_signal(sig, handler);
}

teapot_signal_function bsd_signal__teapot_wrapper__(int sig, teapot_signal_function handler) {
    return signal__teapot_wrapper__(sig, handler);
}

teapot_signal_function ssignal__teapot_wrapper__(int sig, teapot_signal_function handler) {
    return signal__teapot_wrapper__(sig, handler);
}

teapot_signal_function sigset__teapot_wrapper__(int sig, teapot_signal_function handler) {

    sigset_t set, previous_mask;
    struct sigaction previous;
    sigemptyset(&set);
    if (sigaddset(&set, sig) != 0)
        return SIG_ERR;
    if (handler == SIG_HOLD) {
        if (sigprocmask(SIG_BLOCK, &set, &previous_mask) != 0)
            return SIG_ERR;
        if (sigismember(&previous_mask, sig) == 1)
            return SIG_HOLD;
        if (sigaction__teapot_wrapper__(sig, NULL, &previous) != 0)
            return SIG_ERR;
        return previous.sa_handler;
    }

    /* sigset is persistent, with an empty sa_mask and no action flags. It is
     * not glibc's one-shot __sysv_signal interface. Install before unblocking,
     * and return SIG_HOLD when the previous thread mask held this signal. */
    struct sigaction action = { .sa_handler = handler, .sa_flags = 0 };
    sigemptyset(&action.sa_mask);
    if (sigaction__teapot_wrapper__(sig, &action, &previous) != 0)
        return SIG_ERR;
    if (sigprocmask(SIG_UNBLOCK, &set, &previous_mask) != 0)
        return SIG_ERR;
    return sigismember(&previous_mask, sig) == 1 ? SIG_HOLD : previous.sa_handler;
}

/* Written once at setup and never read during simulation, so it may stay
 * outside the protected section. File scope gives it the same symbol name with
 * every compiler, for the protected-globals test. */
static stack_t signal_stack;

void setup_signal_handler() {
    // The kernel's frame (including extended CPU state) and the handler's
    // stack are separate budgets. SIGSTKSZ alone can leave too little room
    // for forwarding an application's handler on older libc/newer CPUs.
    // Keep this independent of application/scratch stacks and guard both ends.
    stack_t *const ss = &signal_stack;
    if (ss->ss_sp == NULL) {
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
        ss->ss_sp = stack;
        ss->ss_size = size;
    }
    struct sigaction sa = {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_ONSTACK | SA_SIGINFO
    };
    if (sigaltstack(ss, NULL) != 0) {
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
