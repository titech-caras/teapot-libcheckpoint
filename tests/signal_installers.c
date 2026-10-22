#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
/* Actual source/private saved actions; only checkpoint assembly is stubbed. */
#include "../src/signal_handler.c"
scratchpad_t scratchpad;
uint64_t checkpoint_cnt;
volatile uint64_t in_restore_memlog;
void restore_checkpoint_SIGSEGV(void) { abort(); }
void restore_checkpoint_after_memlog(void) { abort(); }
void restore_checkpoint_memlog(void) { abort(); }
static int owned_signal;
static volatile sig_atomic_t calls, deferred;
static void first(int sig) {
    sigset_t set;
    assert(sig == owned_signal || sig == SIGUSR1);
    assert(sigprocmask(SIG_SETMASK, NULL, &set) == 0);
    deferred = sigismember(&set, sig);++calls;
}
static void second(int sig) { (void)sig; }
static void reset(void) {
    struct sigaction action = {.sa_handler = SIG_DFL};sigset_t set;
    sigemptyset(&set);assert(sigprocmask(SIG_SETMASK, &set, NULL) == 0);
    assert(sigaction(owned_signal, &action, NULL) == 0);setup_signal_handler();calls = deferred = 0;
}
static void runtime_owned(int restart) {
    struct sigaction actual;assert(sigaction(owned_signal, NULL, &actual) == 0);
    assert(actual.sa_sigaction == signal_handler);
    assert((actual.sa_flags & (SA_ONSTACK | SA_SIGINFO)) == (SA_ONSTACK | SA_SIGINFO));
    assert(!(actual.sa_flags & (SA_RESETHAND | SA_NODEFER)));
    assert(!!(actual.sa_flags & SA_RESTART) == restart);
    for (int sig = 1; sig < NSIG; ++sig)
        if (sig != SIGKILL && sig != SIGSTOP && sig != 32 && sig != 33)
            assert(sigismember(&actual.sa_mask, sig) == 1);
}
typedef teapot_signal_function (*installer)(int, teapot_signal_function);
static void installer_test(installer wrapper, installer libc_entry, int flags, int self_mask) {
    reset();assert(wrapper(owned_signal, first) == SIG_DFL);runtime_owned(!!(flags & SA_RESTART));
    struct saved_signal_action *saved = saved_action_for_signal(owned_signal);
    assert(saved && saved->valid && saved->action.sa_handler == first && saved->action.sa_flags == flags);
    for (int sig = 1; sig < NSIG; ++sig)
        assert(sigismember(&saved->action.sa_mask, sig) == (sig == owned_signal && self_mask));
    assert(wrapper(owned_signal, second) == first);assert(wrapper(owned_signal, first) == second);
    raise(owned_signal);assert(calls == 1 && deferred == !(flags & SA_NODEFER));
    runtime_owned(!!(flags & SA_RESTART));
    assert(saved->action.sa_handler == ((flags & SA_RESETHAND) ? SIG_DFL : first));
    if (flags & SA_RESETHAND) {
        assert(saved->action.sa_flags == 0);pid_t child = fork();assert(child >= 0);
        if (!child) { raise(owned_signal);_exit(99); }
        int status;assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == owned_signal);
    } else { raise(owned_signal);assert(calls == 2); }
    struct sigaction action = {.sa_handler = SIG_DFL}, raw, wrapped;
    assert(sigaction(SIGUSR1, &action, NULL) == 0);assert(libc_entry(SIGUSR1, first) == SIG_DFL);
    assert(sigaction(SIGUSR1, NULL, &raw) == 0);assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(wrapper(SIGUSR1, first) == SIG_DFL);assert(sigaction(SIGUSR1, NULL, &wrapped) == 0);
    assert(raw.sa_handler == wrapped.sa_handler && raw.sa_flags == wrapped.sa_flags);
    for (int sig = 1; sig < NSIG; ++sig)
        assert(sigismember(&raw.sa_mask, sig) == sigismember(&wrapped.sa_mask, sig));
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    if (wrapper != sigset__teapot_wrapper__) {
        errno = 0;assert(wrapper(owned_signal, SIG_ERR) == SIG_ERR && errno == EINVAL);
    }
    errno = 0;assert(wrapper(0, first) == SIG_ERR && errno == EINVAL);
    errno = 0;assert(wrapper(SIGKILL, first) == SIG_ERR && errno == EINVAL);
}
static void holds(void) {
    reset();assert(sigset__teapot_wrapper__(owned_signal, first) == SIG_DFL);
    assert(sigset__teapot_wrapper__(owned_signal, SIG_HOLD) == first);
    assert(sigset__teapot_wrapper__(owned_signal, SIG_HOLD) == SIG_HOLD);runtime_owned(0);
    struct saved_signal_action *saved = saved_action_for_signal(owned_signal);
    assert(saved->action.sa_handler == first && saved->action.sa_flags == 0);sigset_t set;
    assert(sigprocmask(SIG_SETMASK, NULL, &set) == 0 && sigismember(&set, owned_signal) == 1);
    assert(sigset__teapot_wrapper__(owned_signal, second) == SIG_HOLD);
    assert(sigprocmask(SIG_SETMASK, NULL, &set) == 0 && sigismember(&set, owned_signal) == 0);
    assert(sigset__teapot_wrapper__(owned_signal, first) == second);
    assert(sigset__teapot_wrapper__(owned_signal, SIG_HOLD) == first);
    assert(sigset__teapot_wrapper__(owned_signal, SIG_IGN) == SIG_HOLD);
    assert(sigset__teapot_wrapper__(owned_signal, SIG_DFL) == SIG_IGN);runtime_owned(0);
    assert(sigset__teapot_wrapper__(SIGUSR1, first) == SIG_DFL);
    assert(sigset__teapot_wrapper__(SIGUSR1, SIG_HOLD) == first);
    assert(sigset__teapot_wrapper__(SIGUSR1, SIG_HOLD) == SIG_HOLD);
    assert(sigset__teapot_wrapper__(SIGUSR1, SIG_DFL) == SIG_HOLD);
}
int main(void) {
    struct rlimit core = {0,0};assert(setrlimit(RLIMIT_CORE, &core) == 0);
    int flags = SA_RESETHAND | SA_NODEFER;
#ifdef SA_INTERRUPT
    flags |= SA_INTERRUPT;
#endif
    const int owned[] = {SIGILL,SIGSEGV,SIGFPE,SIGBUS};
    for (unsigned i=0;i<sizeof(owned)/sizeof(owned[0]);++i) {
    owned_signal=owned[i];
    installer_test(__sysv_signal__teapot_wrapper__, __sysv_signal, flags, 0);
    installer_test(sysv_signal__teapot_wrapper__, sysv_signal, flags, 0);
    installer_test(bsd_signal__teapot_wrapper__, bsd_signal, SA_RESTART, 1);
    installer_test(ssignal__teapot_wrapper__, ssignal, SA_RESTART, 1);
    installer_test(sigset__teapot_wrapper__, sigset, 0, 0);holds();
    }
    puts("PASS: five installers, exact saved actions/returns, one-shot forwarding, kernel ownership and SIG_HOLD");
    return 0;
}
