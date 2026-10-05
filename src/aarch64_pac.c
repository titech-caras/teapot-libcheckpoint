/* Experimental PAC backend: signed return addresses next to the BTI mode.
 *
 * The rewriter (--target-identification aarch64-bti-pac) inserts non-hint
 * PACIA/AUTIA pairs around every eligible return sequence. This file activates
 * PAC after BTI, detects FEAT_FPAC, and routes authentication faults: inside
 * simulation they roll back through the existing malformed-target path, at
 * depth zero the original application signal is forwarded unchanged.
 *
 * The build flag is shared with the BTI backend (TEAPOT_EXPERIMENTAL_AARCH64_BTI)
 * so a BTI-only runtime cannot satisfy a PAC rewrite: the rewriter's
 * initialization patch references libcheckpoint_enable_aarch64_bti_pac, which
 * only this object defines, and this object is pulled from the archive only
 * when that reference exists.
 */
#define _GNU_SOURCE
#include "checkpoint.h"
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef HWCAP_PACA
#define HWCAP_PACA (1UL << 30)
#endif
#ifndef PR_PAC_GET_ENABLED_KEYS
#define PR_PAC_GET_ENABLED_KEYS 61
#endif
#ifndef PR_PAC_APIAKEY
#define PR_PAC_APIAKEY (1UL << 0)
#endif
/* PSTATE.BTYPE, set by a branch-target exception; a BTI fault is not a PAC
 * authentication failure even when the branch lands on an AUT word. */
#define PAC_BTYPE_MASK (3UL << 10)

/* Provided by aarch64_bti.c, with which this object is always built. */
extern void teapot_aarch64_bti_activate(void);

/* Non-hint PACIA/AUTIA with the SP modifier. The runtime build does not enable
 * the pauth architecture extension, so encode the two instructions directly
 * (pacia x0, sp = 0xdac103e0, autia x0, sp = 0xdac113e0). */
static inline uint64_t pacia_x0_sp(uint64_t value) {
    register uint64_t x0 asm("x0") = value;
    asm volatile(".inst 0xdac103e0" : "+r"(x0));
    return x0;
}

static inline uint64_t autia_x0_sp(uint64_t value) {
    register uint64_t x0 asm("x0") = value;
    asm volatile(".inst 0xdac113e0" : "+r"(x0));
    return x0;
}

static uint64_t pac_active LIBCHECKPOINT_PROTECTED_SECTION;
static uint64_t pac_fpac LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_pac_rollbacks LIBCHECKPOINT_PROTECTED_SECTION;
LIBCHECKPOINT_ASSERT_PROTECTED(pac_active);
LIBCHECKPOINT_ASSERT_PROTECTED(pac_fpac);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_pac_rollbacks);

extern void restore_checkpoint_MALFORMED_INDIRECT_BR(void);

static void fail(const char *reason) {
    fprintf(stderr, "[teapot-bti-pac] refusing activation: %s\n", reason);
    exit(78);
}

/* True for the authentication half of a PAC pair: AUTIA/AUTIB/AUTIZA/AUTIZB,
 * AUTIASP/AUTIBSP and RETAA/RETAB. Used both to classify an FPAC trap and to
 * keep the BTI landing classifier away from PAC faults. */
bool teapot_aarch64_pac_auth_word(uint32_t word) {
    if (word == UINT32_C(0xd50323bf) || word == UINT32_C(0xd50323ff))
        return true; /* autiasp, autibsp */
    if (word == UINT32_C(0xd65f0bff) || word == UINT32_C(0xd65f0fff))
        return true; /* retaa, retab */
    return (word & UINT32_C(0xffffd800)) == UINT32_C(0xdac11000);
}

bool teapot_aarch64_pac_signal(int sig, siginfo_t *info, void *context) {
    if (!pac_active || sig != SIGILL || !info || info->si_code <= 0)
        return false;
    if (info->si_code != ILL_ILLOPN && info->si_code != ILL_ILLOPC)
        return false;
    ucontext_t *uc = context;
    uintptr_t pc = uc->uc_mcontext.pc;
    if (pc & 3)
        return false;
    if (uc->uc_mcontext.pstate & PAC_BTYPE_MASK)
        return false; /* branch-target exception: the BTI classifier owns it */
    /* Code/site provenance: only an authentication instruction at the saved PC
     * is a PAC failure. Kernel and QEMU may report si_addr differently, so an
     * addressed report must agree when one is present. */
    if (info->si_addr && (uintptr_t)info->si_addr != pc)
        return false;
    if (!teapot_aarch64_pac_auth_word(*(const uint32_t *)pc))
        return false;
    if (!checkpoint_cnt)
        return false; /* depth zero: forward the original fault unchanged */
    teapot_pac_rollbacks++;
    uc->uc_mcontext.pc = (uintptr_t)restore_checkpoint_MALFORMED_INDIRECT_BR;
    return true;
}

uint64_t teapot_aarch64_pac_fpac(void) {
    return pac_fpac;
}

static void fpac_probe_signal(int sig, siginfo_t *info, void *context) {
    (void)context;
    _exit(sig == SIGILL && (info->si_code == ILL_ILLOPN || info->si_code == ILL_ILLOPC) ? 0 : 5);
}

/* Child body for the FEAT_FPAC probe: deliberately corrupt one PAC bit of a
 * signed value and authenticate it. FPAC traps; without FPAC the authentication
 * returns a poisoned pointer. Bit 54 is a PAC bit at every supported
 * virtual-address size (up to 52 bits). Bit 62 was used before, but where TBI
 * covers instruction addresses, as in QEMU user mode, it is a tag bit: flipping
 * it only changes the signed input, the 7-bit PAC still matched in about 1 of
 * 128 runs, and FPAC was reported absent. */
static int fpac_probe_child(void) {
    const uint64_t canary = UINT64_C(0x00007f123456789a);
    uint64_t signed_value = pacia_x0_sp(canary);
    uint64_t corrupted = signed_value ^ (UINT64_C(1) << 54);
    volatile uint64_t result = autia_x0_sp(corrupted);
    return result == canary ? 3 : 1;
}

/* Forked so a trap is observed without changing this process's dispositions. */
static bool fpac_probe(void) {
    pid_t child = fork();
    if (child < 0)
        fail("cannot run the FPAC probe");
    if (!child) {
        struct sigaction action = {.sa_sigaction = fpac_probe_signal, .sa_flags = SA_SIGINFO};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGILL, &action, NULL)) _exit(4);
        _exit(fpac_probe_child());
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status))
        fail("FPAC probe did not report a result");
    if (WEXITSTATUS(status) == 0)
        return true;
    if (WEXITSTATUS(status) == 1)
        return false;
    fail("FPAC probe observed neither a trap nor a poisoned pointer");
    return false;
}

static void print_pac_counters(void) {
    if (!pac_active)
        return;
    fprintf(stderr, "[teapot-bti-pac] exit: fpac rollbacks=%llu\n",
            (unsigned long long)teapot_pac_rollbacks);
}

void teapot_aarch64_pac_activate(void) {
    if (pac_active)
        return;
    if (!(getauxval(AT_HWCAP) & HWCAP_PACA))
        fail("CPU/OS does not advertise PAC");
    /* A tag may be zero and modifiers may collide, so key state is checked
     * where the kernel exposes it and always proved by a sign/auth roundtrip. */
    if (prctl(PR_PAC_GET_ENABLED_KEYS, 0, 0, 0, 0) >= 0 &&
            !(prctl(PR_PAC_GET_ENABLED_KEYS, 0, 0, 0, 0) & PR_PAC_APIAKEY))
        fail("APIAKey is not enabled");
    static const uint64_t canaries[] = {
        UINT64_C(0x00007f123456789a), UINT64_C(0x0000000012345678), UINT64_C(0x0000deadbeef0001)};
    bool changed = false;
    for (size_t i = 0; i < sizeof canaries / sizeof canaries[0]; i++) {
        uint64_t signed_value = pacia_x0_sp(canaries[i]);
        if (autia_x0_sp(signed_value) != canaries[i])
            fail("PAC sign/auth roundtrip failed");
        changed |= signed_value != canaries[i];
    }
    if (!changed)
        fail("PAC key did not change a signed pointer");
    pac_fpac = fpac_probe();
    pac_active = 1;
    atexit(print_pac_counters);
    fprintf(stderr, "[teapot-bti-pac] active: FPAC %s; signed return addresses enforced\n",
            pac_fpac ? "present" : "absent");
}

static void pac_preinit(void) {
    /* The rewriter emits this weak alias only in --target-identification
     * aarch64-bti-pac. Absent (ordinary/BTI-only builds, runtime unit fixtures),
     * early PAC activation stays off. */
    extern void teapot_aarch64_bti_pac_rewrite_marker(void) __attribute__((weak));
    if (!teapot_aarch64_bti_pac_rewrite_marker)
        return;
    extern void libcheckpoint_prepare_aarch64_bti_pac_components(void);
    libcheckpoint_prepare_aarch64_bti_pac_components();
}

/* Present exactly when a PAC rewrite references this object (the initialization
 * patch or the component adapter), so PAC is active before constructors run. */
__attribute__((used, section(".preinit_array")))
static void (*const pac_preinit_entry)(void) = pac_preinit;

LIBCHECKPOINT_PRESERVE_MOST void libcheckpoint_enable_aarch64_bti_pac(int argc, char **argv) {
    libcheckpoint_enable(argc, argv);
    teapot_aarch64_bti_activate();
    teapot_aarch64_pac_activate();
}
