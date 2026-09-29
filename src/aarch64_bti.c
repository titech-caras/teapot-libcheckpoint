/* Experimental backend. No ELF-wide BTI note: normal pages alone are guarded. */
#define _GNU_SOURCE
#include "checkpoint.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif
#ifndef HWCAP2_BTI
#define HWCAP2_BTI (1UL << 17)
#endif
#define BTI_JC UINT32_C(0xd50324df)
#define SECOND_MAGIC UINT32_C(0xd280a29f)
#define BTYPE_MASK (3UL << 10)

/* Weak declarations allow existing runtime unit fixtures, which do not enable
 * this backend, to link unchanged. Activation checks every required bound. */
extern char __teapot_bti_guard_start[] __attribute__((weak));
extern char __teapot_bti_guard_end[] __attribute__((weak));
extern char __teapot_bti_text_start[] __attribute__((weak));
extern char __teapot_bti_text_end[] __attribute__((weak));
extern char __teapot_bti_transient_start[] __attribute__((weak));
extern char __teapot_bti_transient_end[] __attribute__((weak));
extern char teapot_bti_probe_valid[], teapot_bti_probe_invalid[];
extern char teapot_bti_probe_brk[], teapot_bti_probe_hlt[];
extern int teapot_bti_call_probe(void *);
extern void restore_checkpoint_MALFORMED_INDIRECT_BR(void);
extern bool teapot_aarch64_pac_auth_word(uint32_t) __attribute__((weak));

static uint64_t bti_active LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_bti_normal_resumes LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_bti_rollbacks LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(bti_active);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_bti_normal_resumes);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_bti_rollbacks);

/* Window bounds for report-site patching; zero until activation succeeds. */
uint64_t teapot_bti_text_lo, teapot_bti_text_hi, teapot_bti_copy_lo, teapot_bti_copy_hi;

static void fail(const char *reason) {
    fprintf(stderr, "[teapot-bti] refusing activation: %s\n", reason);
    exit(78);
}

/* Non-trapping BTI-compatible instructions across BR/BLR register variants
 * and SCTLR.BT settings. Every one must be a complete inserted marker. */
static int hardware_landing(uint32_t word) {
    return word == BTI_JC || word == 0xd503245fU || word == 0xd503249fU ||
           word == 0xd503233fU || word == 0xd503237fU;
}

static int trap_landing(uint32_t word) {
    return (word & 0xffe0001fU) == 0xd4200000U ||
           (word & 0xffe0001fU) == 0xd4400000U;
}

static int guarded_branch_context(const siginfo_t *info, const ucontext_t *uc,
                                  uintptr_t start, uintptr_t end) {
    uintptr_t pc = uc->uc_mcontext.pc;
    return info && pc >= start && pc < end && !(pc & 3) &&
           (uintptr_t)info->si_addr == pc && (uc->uc_mcontext.pstate & BTYPE_MASK);
}

static int illegal_instruction(int sig, const siginfo_t *info) {
    return sig == SIGILL &&
           (info->si_code == ILL_ILLOPC || info->si_code == ILL_ILLOPN);
}

static int trap_signal(uint32_t word, int sig, const siginfo_t *info) {
    if ((word & 0xffe0001fU) == 0xd4200000U)
        return sig == SIGTRAP && info->si_code == TRAP_BRKPT;
    return (word & 0xffe0001fU) == 0xd4400000U && illegal_instruction(sig, info);
}

static uintptr_t probe_target;
static void probe_signal(int sig, siginfo_t *info, void *context) {
    if (!guarded_branch_context(info, context, probe_target, probe_target + 4))
        _exit(3);
    uint32_t word = *(const uint32_t *)probe_target;
    _exit((trap_landing(word) ? trap_signal(word, sig, info) :
           illegal_instruction(sig, info)) ? 0 : 3);
}

bool teapot_aarch64_bti_signal(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (!bti_active || !guarded_branch_context(info, uc,
            (uintptr_t)__teapot_bti_guard_start, (uintptr_t)__teapot_bti_guard_end))
        return false;
    uint32_t word = *(const uint32_t *)uc->uc_mcontext.pc;
    if (teapot_aarch64_pac_auth_word && teapot_aarch64_pac_auth_word(word))
        return false; /* PAC authentication faults use their own classifier */
    if (trap_landing(word)) {
        /* BRK/HLT win over BTI, including data words with those encodings.
         * They cannot execute past the target during simulation. At depth
         * zero forward the ORIGINAL trap/context; this is not a BTI retry. */
        if (!checkpoint_cnt || !trap_signal(word, sig, info))
            return false;
    } else if (!illegal_instruction(sig, info)) {
        return false;
    }
    // The saved PC is the destination, not the originating branch. Simulation
    // state, never a guess from the destination range, decides the slow path.
    uc->uc_mcontext.pstate &= ~BTYPE_MASK;
    if (checkpoint_cnt) {
        teapot_bti_rollbacks++;
        uc->uc_mcontext.pc = (uintptr_t)restore_checkpoint_MALFORMED_INDIRECT_BR;
    } else {
        teapot_bti_normal_resumes++;
        // Resume the same instruction, not PC+4. Genuine instruction faults
        // on its second execution have BTYPE=0 and follow ordinary forwarding.
    }
    return true;
}

/* Exit-time counters so parity runs can attribute report differences. */
static void print_bti_counters(void) {
    if (!bti_active)
        return;
    fprintf(stderr, "[teapot-bti] exit: malformed rollbacks=%llu normal resumes=%llu\n",
            (unsigned long long)teapot_bti_rollbacks,
            (unsigned long long)teapot_bti_normal_resumes);
}

void teapot_aarch64_bti_activate(void) {
    if (bti_active)
        return;
    uintptr_t lo = (uintptr_t)__teapot_bti_guard_start;
    uintptr_t hi = (uintptr_t)__teapot_bti_guard_end;
    uintptr_t text = (uintptr_t)__teapot_bti_text_start;
    uintptr_t end = (uintptr_t)__teapot_bti_text_end;
    uintptr_t shadow = (uintptr_t)__teapot_bti_transient_start;
    uintptr_t shadow_end = (uintptr_t)__teapot_bti_transient_end;
    long pages = sysconf(_SC_PAGESIZE);
    if (!lo || !text || !shadow || pages <= 0 || pages > 65536 ||
            (pages & (pages - 1)) || lo % pages || hi % pages ||
            hi <= lo || text < lo || end <= text || end > hi - 8 ||
            (text & 3) || (end & 3) || shadow_end <= shadow ||
            (shadow & 3) || (shadow_end & 3) || shadow < end ||
            shadow_end > hi - 8 ||
            ((uintptr_t)teapot_aarch64_bti_activate >= lo &&
             (uintptr_t)teapot_aarch64_bti_activate < hi))
        fail("missing or non-isolated linker bounds");
    if (!(getauxval(AT_HWCAP2) & HWCAP2_BTI))
        fail("CPU/OS does not advertise BTI");
    size_t markers = 0;
    const uintptr_t ranges[2][2] = {{text, end}, {shadow, shadow_end}};
    for (size_t range_index = 0; range_index < 2; range_index++) {
        for (uintptr_t pc = ranges[range_index][0]; pc < ranges[range_index][1]; pc += 4) {
            const uint32_t *words = (const uint32_t *)pc;
            if (hardware_landing(words[0])) {
                if (words[0] != BTI_JC || words[1] != SECOND_MAGIC)
                    fail("normal or copied text has an unmatched non-trapping hardware landing (BTI/PAC)");
                markers++;
            }
        }
    }
    if (!markers)
        fail("normal and copied text contain no transformed BTI markers");
    const uintptr_t targets[] = {(uintptr_t)teapot_bti_probe_invalid,
        (uintptr_t)teapot_bti_probe_brk, (uintptr_t)teapot_bti_probe_hlt};
    uintptr_t valid = (uintptr_t)teapot_bti_probe_valid;
    if (valid < shadow_end || valid > hi - 4 || (valid & 3))
        fail("enforcement probe is not isolated outside application targets");
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++)
        if (targets[i] < shadow_end || targets[i] > hi - 4 || (targets[i] & 3))
            fail("enforcement probe is not isolated outside application targets");
    // Both code sequences must execute before page protection is enabled.
    if (teapot_bti_call_probe(teapot_bti_probe_valid) != 42 ||
            teapot_bti_call_probe(teapot_bti_probe_invalid) != 42)
        fail("unguarded control probe failed");
    if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_EXEC | PROT_BTI))
        fail("mprotect(PROT_BTI) failed");
    // Prove enforcement on the actual final normal-code mapping, not merely
    // an anonymous scratch page or a property note. BRK/HLT must also deliver
    // the trap-priority signal/context used above. No checkpoint is active.
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        pid_t child = fork();
        if (child < 0)
            fail("cannot run enforcement probe");
        if (!child) {
            probe_target = targets[i];
            struct sigaction action = {.sa_sigaction = probe_signal, .sa_flags = SA_SIGINFO};
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGILL, &action, NULL) || sigaction(SIGTRAP, &action, NULL)) _exit(4);
            if (teapot_bti_call_probe(teapot_bti_probe_valid) != 42) _exit(5);
            teapot_bti_call_probe((void *)probe_target);
            _exit(6);
        }
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fail("actual normal mapping did not enforce invalid/trapping landings");
    }
    teapot_bti_text_lo = text;
    teapot_bti_text_hi = end;
    teapot_bti_copy_lo = shadow;
    teapot_bti_copy_hi = shadow_end;
    bti_active = 1;
    atexit(print_bti_counters);
    fprintf(stderr, "[teapot-bti] active: %zu validated markers in normal and copied text, "
            "%zu guarded bytes; range/return checks retained\n",
            markers, (size_t)(hi - lo));
}

LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_enable_aarch64_bti(int argc, char **argv) {
    libcheckpoint_enable(argc, argv);
    teapot_aarch64_bti_activate();
}
