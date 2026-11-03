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
#include <sys/syscall.h>
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
#ifndef DISABLE_DIFT_RUNTIME
static unsigned char tagged_memory;
static void set_register_shadow(unsigned tags, unsigned queued, unsigned pending) {
    for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
        dift_reg_tags[i] = tags + i;
        dift_reg_queued_tags[i] = queued ? queued + i : 0;
    }
    dift_reg_queue_pending[0] = pending;
}
static void on_tags(int sig) {
    assert(sig == SIGUSR1 && checkpoint_cnt == 0 && !in_restore_memlog);
    /* Pending work belongs to the interrupted instruction, not this handler. */
    assert(dift_reg_queue_pending[0] == 0);
    for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i)
        assert(dift_reg_queued_tags[i] == 0);
    set_register_shadow(64, 128, 1);
    DIFT_MEM_TAG(&tagged_memory) = TAG_SECRET_INDIRECT;
    if (mode == 7) {
        assert(checkpoint_rollback_probe(handler_checkpoint_stack) == 1);
        for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
            assert(dift_reg_tags[i] == 64 + i);
            assert(dift_reg_queued_tags[i] == 0);
        }
        assert(dift_reg_queue_pending[0] == 0);
        set_register_shadow(96, 160, 1);
    }
    ++calls;
}
static void on_tags_info(int sig, siginfo_t *info, void *context) {
    assert(info->si_code == SI_TKILL && !teapot_signal_runtime_busy(context));
    on_tags(sig);
}
static void check_signal_tags(void) {
    for (unsigned siginfo = 0; siginfo < 2; ++siginfo) {
        struct sigaction action = {.sa_handler = on_tags};
        if (siginfo) {
            action.sa_sigaction = on_tags_info;
            action.sa_flags = SA_SIGINFO;
        }
        assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
        /* Empty, pending, and partially consumed queues (pending already zero). */
        for (unsigned queue = 0; queue < 3; ++queue) {
            set_register_shadow(1, queue ? 192 : 0, queue == 1);
            DIFT_MEM_TAG(&tagged_memory) = TAG_SECRET;
            assert(raise(SIGUSR1) == 0);
            for (unsigned i = 0; i < DIFT_REG_TAGS_SIZE; ++i) {
                assert(dift_reg_tags[i] == 1 + i);
                assert(dift_reg_queued_tags[i] == (queue ? 192 + i : 0));
            }
            assert(dift_reg_queue_pending[0] == (queue == 1));
            assert(DIFT_MEM_TAG(&tagged_memory) == TAG_SECRET_INDIRECT);
        }
    }
    assert(calls == 6);
    puts("PASS: interrupted register tags and queue restored; handler memory tags retained");
}
#endif
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
    if (mode == 5) {
        assert(checkpoint_rollback_probe(handler_checkpoint_stack) == 1);
        /* At the stub's final jump, a nested signal frame on the same stack may
         * cover the released stub frame, and a handler that rolls back sets
         * its own resume target. Neither may change where the stub goes. */
#if defined(__x86_64__)
        ((uint64_t *)((ucontext_t *)context)->uc_mcontext.gregs[REG_RSP])[1] = 0;
#endif
        teapot_signal_resume_target = 0;
    }
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
#ifndef DISABLE_DIFT_RUNTIME
    if (mode == 7) {
        set_register_shadow(128, 64, 1);
        restore_checkpoint_ROB_LEN();
    }
#endif
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
/* A rewritten handler's own sites and spills reuse the global slots that the
 * interrupted code may hold live; forwarding must hand them back unchanged. */
static const struct { size_t offset, size; } test_slots[] = {
    {0, 64},
    {SCRATCHPAD_FIRST_SPILL_OFFSET, X64_REP_STATE_OFFSET + X64_REP_STATE_SIZE - SCRATCHPAD_FIRST_SPILL_OFFSET},
    {TEXT_DIFT_LLVM_SCRATCH_SAVE_OFFSET, 4096},
};
static void fill_slots(uint64_t base) {
    for (size_t i = 0; i < CHECKPOINT_TARGET_METADATA_SIZE / 8; ++i)
        checkpoint_target_metadata[i] = base + i;
    for (size_t r = 0; r < sizeof(test_slots) / sizeof(test_slots[0]); ++r)
        for (size_t i = 0; i < test_slots[r].size / 8; ++i)
            ((uint64_t *)(scratchpad + test_slots[r].offset))[i] = base + (r + 1) * 0x10000 + i;
}
static void on_slots(int sig) { assert(sig == SIGUSR1); fill_slots(0xbad00000); ++calls; }
static void check_slots(void) {
    struct sigaction action = {.sa_handler = on_slots};
    assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
    fill_slots(0x100000);
    assert(raise(SIGUSR1) == 0);
    assert(calls == 1);
    for (size_t i = 0; i < CHECKPOINT_TARGET_METADATA_SIZE / 8; ++i)
        assert(checkpoint_target_metadata[i] == 0x100000 + i);
    for (size_t r = 0; r < sizeof(test_slots) / sizeof(test_slots[0]); ++r)
        for (size_t i = 0; i < test_slots[r].size / 8; ++i)
            assert(((uint64_t *)(scratchpad + test_slots[r].offset))[i] == 0x100000 + (r + 1) * 0x10000 + i);
    puts("PASS: checkpoint-site arguments and scratch slots restored after the handler");
}
#if defined(__aarch64__)
/* Forwarded handlers run on the runtime's alternate stack. Rewritten AArch64
 * code spills to sp - AARCH64_SHADOW_STACK_SIZE, which the runtime must own. */
static void on_altstack(int sig) {
    stack_t ss;
    assert(sig == SIGUSR1 && sigaltstack(NULL, &ss) == 0 && (ss.ss_flags & SS_ONSTACK));
    uintptr_t sp = ((uintptr_t)__builtin_frame_address(0) - 1024) & ~(uintptr_t)15;
    assert(sp - (uintptr_t)ss.ss_sp < ss.ss_size);
    /* That window is a mapping of its own beside this stack, not whatever
     * else happens to lie 8 MiB below it (under QEMU, another mapping does). */
    uintptr_t window = (uintptr_t)ss.ss_sp - AARCH64_SHADOW_STACK_SIZE, start = 0, end = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    assert(maps);
    unsigned long long low, high;
    while (fscanf(maps, "%llx-%llx%*[^\n]", &low, &high) == 2)
        if (low <= window && window < high) { start = low; end = high; break; }
    fclose(maps);
    assert(start >= window - 4096 && end >= window + ss.ss_size && end <= (uintptr_t)ss.ss_sp);
    /* The real entry on this stack reads and writes that window. */
    assert(checkpoint_rollback_probe((void *)sp) == 1);
    ++calls;
}
static void check_altstack(void) {
    struct sigaction action = {.sa_handler = on_altstack};
    assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
    assert(raise(SIGUSR1) == 0);
    assert(calls == 1);
    puts("PASS: handler checkpoint on the alternate stack uses a runtime-owned window");
}
#endif
/* An application stack of 8 KiB (glibc's old SIGSTKSZ) above a guard page.
 * Forwarding needs more, so handlers must keep using the runtime's stack. */
#define APPLICATION_STACK_SIZE 8192
static char *application_stack;
static void on_application_stack(int sig) {
    stack_t ss, kernel;
    assert(sig == SIGUSR1 && sigaltstack(NULL, &kernel) == 0 && (kernel.ss_flags & SS_ONSTACK));
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    assert(sp - (uintptr_t)kernel.ss_sp < kernel.ss_size);
    assert(sp - (uintptr_t)application_stack >= APPLICATION_STACK_SIZE);
    assert(sigaltstack__teapot_wrapper__(NULL, &ss) == 0);
    assert(ss.ss_sp == application_stack && ss.ss_size == APPLICATION_STACK_SIZE && ss.ss_flags == SS_ONSTACK);
#if defined(__aarch64__)
    assert(checkpoint_rollback_probe((void *)((sp - 1024) & ~(uintptr_t)15)) == 1);
#endif
    ++calls;
}
static void check_application_altstack(void) {
    long page = sysconf(_SC_PAGESIZE);
    char *mapping = mmap(NULL, page + APPLICATION_STACK_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    application_stack = mapping + page;
    assert(mprotect(application_stack, APPLICATION_STACK_SIZE, PROT_READ | PROT_WRITE) == 0);
    stack_t runtime, ss = {.ss_sp = application_stack, .ss_size = APPLICATION_STACK_SIZE}, old;
    assert(sigaltstack(NULL, &runtime) == 0);
    assert(sigaltstack__teapot_wrapper__(&ss, &old) == 0);
    struct sigaction action = {.sa_handler = on_application_stack, .sa_flags = SA_ONSTACK};
    assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
    assert(raise(SIGUSR1) == 0);
    assert(calls == 1);
    assert(old.ss_flags == SS_DISABLE);
    stack_t kernel;
    assert(sigaltstack(NULL, &kernel) == 0 && kernel.ss_sp == runtime.ss_sp && kernel.ss_size == runtime.ss_size);
    assert(sigaltstack__teapot_wrapper__(NULL, &old) == 0);
    assert(old.ss_sp == application_stack && old.ss_size == APPLICATION_STACK_SIZE && old.ss_flags == 0);
    /* The kernel's validation still applies. */
    ss.ss_size = 1;
    assert(sigaltstack__teapot_wrapper__(&ss, NULL) == -1 && errno == ENOMEM);
    ss.ss_flags = 12345;
    assert(sigaltstack__teapot_wrapper__(&ss, NULL) == -1 && errno == EINVAL);
    ss.ss_flags = SS_DISABLE;
    assert(sigaltstack__teapot_wrapper__(&ss, NULL) == 0);
    assert(sigaltstack__teapot_wrapper__(NULL, &old) == 0 && old.ss_flags == SS_DISABLE);
    assert(sigaltstack(NULL, &kernel) == 0 && kernel.ss_sp == runtime.ss_sp);
    puts("PASS: an application's own alternate stack is recorded; handlers use the runtime's");
}
#if defined(__riscv) && __riscv_xlen == 64
static __thread volatile int tls_marker = 0x5a5a;
static void on_tp(int sig) {
    assert(sig == SIGUSR1 && tls_marker == 0x5a5a);
    errno = EINTR;
    ++calls;
}
static void check_thread_pointer(void) {
    struct sigaction action = {.sa_handler = on_tp};
    assert(sigaction__teapot_wrapper__(SIGUSR1, &action, NULL) == 0);
    uintptr_t tp;
    asm volatile("mv %0, tp" : "=r"(tp));
    *(uint64_t *)(scratchpad + RISCV64_ORIGINAL_TP_OFFSET) = tp;
    /* Borrow tp like an RV64 spill; the signal arrives as tgkill returns. */
    register long a0 asm("a0") = getpid();
    register long a1 asm("a1") = gettid();
    register long a2 asm("a2") = SIGUSR1;
    register long a7 asm("a7") = SYS_tgkill;
    asm volatile("mv t2, tp\n"
                 "lla tp, scratchpad + " STR(SCRATCHPAD_FIRST_SPILL_OFFSET) "\n"
                 "ecall\n"
                 "mv tp, t2"
                 : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "t2", "memory");
    assert(a0 == 0 && calls == 1 && tls_marker == 0x5a5a);
    puts("PASS: handler interrupting a borrowed tp sees the thread's own TLS");
}
#endif
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
#ifndef DISABLE_DIFT_RUNTIME
    if (!strcmp(argv[1], "tags") || !strcmp(argv[1], "tags-checkpoint")) {
        mode = !strcmp(argv[1], "tags") ? 6 : 7;
        check_signal_tags(); return 0;
    }
#endif
    if (!strcmp(argv[1], "registers")) { mode = 5; check_registers(); return 0; }
    if (!strcmp(argv[1], "slots")) { check_slots(); return 0; }
    if (!strcmp(argv[1], "application-altstack")) { check_application_altstack(); return 0; }
#if defined(__aarch64__)
    if (!strcmp(argv[1], "altstack")) { check_altstack(); return 0; }
#endif
#if defined(__riscv) && __riscv_xlen == 64
    if (!strcmp(argv[1], "tp")) { check_thread_pointer(); return 0; }
#endif
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
