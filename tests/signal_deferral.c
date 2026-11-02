#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include "checkpoint.h"
#include "signal_handler.h"

extern uintptr_t checkpoint_target_metadata[];
extern int checkpoint_rollback_probe(void *);
extern void restore_checkpoint_ROB_LEN(void) __attribute__((noreturn));
/* Entry fixtures use the real runtime without ASan's startup requirements. */
void __asan_poison_memory_region(void const volatile *p, size_t n) { (void)p; (void)n; }
static volatile sig_atomic_t done, calls;
static uint32_t branch_count;
static int mode;
static void *handler_checkpoint_stack;
int test_signal_at_entry, test_signal_at_restore, test_signal_pid;
static void on_user(int sig, siginfo_t *info, void *context) {
    assert(!teapot_signal_runtime_busy(context));
    assert(checkpoint_cnt == 0 && !in_restore_memlog);
    assert(mode == 4 || info->si_pid == getpid());
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    if (mode == 1) {
        assert(sigismember(&mask, SIGINT) == 1);
        assert(sigismember(&mask, SIGWINCH) == 1);
        assert(sigismember(&mask, sig) == (sig != SIGUSR1));
        assert(info->si_code == (sig == SIGSEGV ? SI_USER : sig == SIGUSR1 ? SI_QUEUE : SI_TKILL));
        if (sig == SIGUSR1) assert(info->si_value.sival_int == 1234);
    }
    if (mode == 5) assert(checkpoint_rollback_probe(handler_checkpoint_stack) == 1);
    ++calls;
}
static void check_restart(void) {
    for (int restart = 0; restart <= 1; ++restart) {
        int fds[2]; assert(pipe(fds) == 0);
        struct sigaction action = {.sa_sigaction = on_user,
            .sa_flags = SA_SIGINFO | (restart ? SA_RESTART : 0)};
        assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
        pid_t parent = getpid(), child = fork(); assert(child >= 0);
        if (!child) {
            close(fds[0]); usleep(20000);
            assert(kill(parent, SIGUSR1) == 0);
            usleep(20000); assert(write(fds[1], "x", 1) == 1); _exit(0);
        }
        close(fds[1]); char byte = 0;
        ssize_t count = read(fds[0], &byte, 1);
        assert(restart ? count == 1 && byte == 'x' : count == -1 && errno == EINTR);
        if (!restart) assert(read(fds[0], &byte, 1) == 1 && byte == 'x');
        close(fds[0]); int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
static void on_child(int sig) { assert(sig == SIGCHLD); }
static void check_child_flags(void) {
    struct sigaction action = {.sa_handler = on_child, .sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT};
    assert(sigaction__teapot_wrapper__(SIGCHLD, &action, NULL) == 0);
    struct sigaction kernel, queried;
    assert(sigaction(SIGCHLD, NULL, &kernel) == 0);
    assert((kernel.sa_flags & action.sa_flags) == action.sa_flags);
    assert(sigaction__teapot_wrapper__(SIGCHLD, NULL, &queried) == 0);
    assert(queried.sa_handler == on_child && queried.sa_flags == action.sa_flags);
    int outcome[2];
    for (int direct = 0; direct < 2; ++direct) {
        if (direct) assert(sigaction(SIGCHLD, &action, NULL) == 0);
        pid_t child = fork(); assert(child >= 0); if (!child) _exit(0);
        int status, result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        outcome[direct] = result < 0 ? -errno : status;
    }
    /* Some QEMU user versions expose SA_NOCLDWAIT in sigaction queries but
     * still reap the child. Require exactly libc's behavior on this runner. */
    assert(outcome[0] == outcome[1]);
    printf("SA_NOCLDWAIT wrapped/direct wait outcome: %d/%d\n", outcome[0], outcome[1]);
}

uint64_t test_signal_register_values[10], test_signal_flags;
extern void test_signal_registers(void), test_signal_registers_return(void);
extern uintptr_t teapot_signal_resume_target;
static void check_registers(void) {
    struct sigaction action = {.sa_sigaction = on_user, .sa_flags = SA_SIGINFO};
    assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    assert(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    assert(raise(SIGUSR1) == 0);
    teapot_pending_signals = UINT64_C(1) << (SIGUSR1 - 1);
    teapot_signal_resume_target = (uintptr_t)test_signal_registers_return;
    checkpoint_target_metadata[CHECKPOINT_TARGET_SCRATCH_REG_ADDR / 8] = 0x12345678;
    test_signal_registers();
    assert(checkpoint_target_metadata[CHECKPOINT_TARGET_SCRATCH_REG_ADDR / 8] == 0x12345678);
    assert(calls == 1 && teapot_pending_signals == 0);
#if defined(__aarch64__)
    for (unsigned i = 0; i < 6; ++i) assert(test_signal_register_values[i] == i + 1);
    assert(test_signal_register_values[6] == test_signal_flags);
#else
    for (unsigned i = 0; i < 7; ++i) assert(test_signal_register_values[i] == i + 1);
#if defined(__x86_64__)
    assert(test_signal_register_values[7] == 0x12345 && test_signal_register_values[8] == 0x6789a);
    assert(test_signal_register_values[9] == test_signal_flags);
#endif
#endif
    puts("PASS: syscall registers/flags preserved across delivery; x64 red zone intact");
}
static void on_alarm(int sig, siginfo_t *info, void *context) {
#ifdef TEST_SIGNAL_BUSY_CHECK
    assert(!teapot_signal_runtime_busy(context));
#else
    (void)context;
#endif
    assert(sig == SIGALRM && info->si_code == SI_KERNEL);
    assert(checkpoint_cnt == 0 && !in_restore_memlog);
    ++calls;
    done = 1;
}
void checkpoint_test_transient_body(void) {
    if (mode == 1) {
        assert(kill(getpid(), SIGSEGV) == 0);
        assert(sigqueue(getpid(), SIGUSR1, (union sigval){.sival_int = 1234}) == 0);
        assert(raise(SIGUSR2) == 0);
        assert(calls == 0);
        restore_checkpoint_ROB_LEN();
    }
    /* Real checkpoint/rollback entries, with enough branches to expose a
     * 100us timer on all three ISAs. Only the handler can end the outer loop. */
    volatile unsigned value = 1;
    for (unsigned i = 0; i < 20000; ++i) {
        if (value & 1) value = value * 3 + 1;
        else value >>= 1;
    }
    restore_checkpoint_ROB_LEN();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    void *stack_top = NULL;
#if defined(__aarch64__)
    size_t size = 2 * AARCH64_SHADOW_STACK_SIZE;
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    stack_top = (char *)mapping + size - 4096;
#endif
    checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR / 8] = (uintptr_t)&branch_count;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG0_SOURCE / 8] = UINTPTR_MAX;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG1_SOURCE / 8] = UINTPTR_MAX;
    libcheckpoint_enabled = true;
    setup_signal_handler();
    handler_checkpoint_stack = stack_top;
    test_signal_pid = getpid();
    if (!strcmp(argv[1], "registers")) { mode = 5; check_registers(); return 0; }
    if (strcmp(argv[1], "timer")) {
        if (!strcmp(argv[1], "restart")) {
            mode = 4; check_restart(); check_child_flags();
            puts("PASS: SA_RESTART read behavior and kernel child flags"); return 0;
        }
        mode = !strcmp(argv[1], "queued") ? 1 : !strcmp(argv[1], "skip") ? 2 : 3;
        struct sigaction action = {.sa_sigaction = on_user, .sa_flags = SA_SIGINFO};
        sigemptyset(&action.sa_mask); sigaddset(&action.sa_mask, SIGWINCH);
        assert(sigaction__teapot_wrapper__(SIGSEGV, &action, NULL) == 0);
        action.sa_flags |= SA_RESETHAND;
        assert(sigaction__teapot_wrapper__(SIGUSR2, &action, NULL) == 0);
        action.sa_flags = SA_SIGINFO | SA_NODEFER;
        assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
        sigset_t original, blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGINT);
        assert(sigprocmask(SIG_BLOCK, &blocked, &original) == 0);
        if (mode == 2) { libcheckpoint_enabled = false; test_signal_at_entry = 1; }
        if (mode == 3) test_signal_at_restore = 1;
        assert(checkpoint_rollback_probe(stack_top) == 1);
        assert(calls == (mode == 1 ? 3 : 1));
        assert(teapot_pending_signals == 0);
        assert(sigprocmask(SIG_SETMASK, NULL, &blocked) == 0);
        assert(sigismember(&blocked, SIGINT) == 1);
        assert(sigismember(&blocked, SIGSEGV) == 0 && sigismember(&blocked, SIGUSR1) == 0 && sigismember(&blocked, SIGUSR2) == 0);
        if (mode == 1) {
            assert(sigaction__teapot_wrapper__(SIGUSR2, NULL, &action) == 0);
            assert(action.sa_handler == SIG_DFL);
        }
        assert(sigprocmask(SIG_SETMASK, &original, NULL) == 0);
        printf("PASS: %s, calls=%d, no busy handler, original mask preserved\n", argv[1], calls);
        return 0;
    }
    struct sigaction action = {.sa_sigaction = on_alarm, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    assert(sigaction__teapot_wrapper__(SIGALRM, &action, NULL) == 0);
    struct itimerval timer = {.it_interval = {.tv_usec = 100}, .it_value = {.tv_usec = 100}};
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    while (!done) assert(checkpoint_rollback_probe(stack_top) == 1);
    timer = (struct itimerval){0};
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    assert(calls > 0 && checkpoint_cnt == 0);
    printf("PASS: timer handler at depth zero, calls=%d checkpoints=%lu\n", calls, simulation_statistics.total_ckpt);
    return 0;
}
