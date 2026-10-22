/* Test-only interposition. Validate the real kernel event at the independently
 * encoded probe, then enter the unmodified production C1 handler. */
#pragma once
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

extern void signal_handler(int, siginfo_t *, void *);
extern int __real_sigaction(int, const struct sigaction *, struct sigaction *);
extern void teapot_pure_illegal_reserved(void);
extern void teapot_pure_illegal_fadd_s(void);
extern void teapot_pure_illegal_fadd_d(void);
extern void teapot_pure_illegal_fmadd_d(void);
static volatile uintptr_t pure_expected_probe_pc;
static volatile uint64_t *pure_verified_kernel_events;

static void pure_sigill_verify(int sig, siginfo_t *info, void *ucontext) {
    if (sig != SIGILL || !info || !ucontext || info->si_signo != SIGILL ||
            info->si_code != ILL_ILLOPC || (uintptr_t)info->si_addr != pure_expected_probe_pc ||
            ((ucontext_t *)ucontext)->uc_mcontext.__gregs[REG_PC] != pure_expected_probe_pc)
        _exit(96);
    if (pure_verified_kernel_events)
        ++*pure_verified_kernel_events;
    signal_handler(sig, info, ucontext);
}

int __wrap_sigaction(int sig, const struct sigaction *action, struct sigaction *old) {
    if (sig == SIGILL && action && (action->sa_flags & SA_SIGINFO) && action->sa_sigaction == signal_handler) {
        struct sigaction verified = *action;
        verified.sa_sigaction = pure_sigill_verify;
        return __real_sigaction(sig, &verified, old);
    }
    return __real_sigaction(sig, action, old);
}

static void pure_sigill_instruction(unsigned kind) {
    void (*const probes[])(void) = {teapot_pure_illegal_reserved, teapot_pure_illegal_fadd_s,
                                   teapot_pure_illegal_fadd_d, teapot_pure_illegal_fmadd_d};
    if (kind < 1 || kind > 4)
        _exit(97);
    pure_expected_probe_pc = (uintptr_t)probes[kind - 1];
    probes[kind - 1]();
}
