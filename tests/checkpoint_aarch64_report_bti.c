/* The report patcher must keep PROT_BTI on a copy page: after patching a call
 * site inside the copy range, an indirect branch to a non-landing word on that
 * page must still raise a branch-target exception. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif
#ifndef PSTATE_BTYPE_MASK
#define PSTATE_BTYPE_MASK (3UL << 10)
#endif

extern void make_report_call_nop(uint64_t address);

/* Strong definitions so the patcher's BTI window checks are active. */
uint64_t teapot_bti_text_lo, teapot_bti_text_hi;
uint64_t teapot_bti_copy_lo, teapot_bti_copy_hi;

static void sigill_handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    (void)sig;
    (void)info;
    _exit((uc->uc_mcontext.pstate & PSTATE_BTYPE_MASK) != 0 ? 0 : 2);
}

int main(void) {
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0)
        return 77;
    uint32_t *page = mmap(NULL, (size_t)size * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 77;
    }
    uint32_t *site = page + 0x40;   /* the patched report call site */
    uint32_t *target = page + 0x80; /* a non-landing branch target */
    *site = 0xd65f03c0;             /* ret: not the expanded report-call pattern */
    *target = 0xd65f03c0;           /* ret: not a BTI landing */
    __builtin___clear_cache((char *)page, (char *)(page + (size / 4) * 2));

    if (mprotect(page, (size_t)size, PROT_READ | PROT_EXEC | PROT_BTI) != 0)
        return 77; /* no BTI support in this kernel or emulator */

    /* The page is inside the copy range, so the patch must restore BTI. */
    teapot_bti_text_lo = 0;
    teapot_bti_text_hi = 0;
    teapot_bti_copy_lo = (uint64_t)(uintptr_t)page;
    teapot_bti_copy_hi = (uint64_t)(uintptr_t)page + (uint64_t)size;
    make_report_call_nop((uintptr_t)site);
    if (*site != 0xd503201f) {
        fputs("report patch did not write a nop\n", stderr);
        return 1;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = sigill_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, NULL) != 0) {
        perror("sigaction");
        return 77;
    }

    asm volatile("blr %0" : : "r"(target)); /* faults iff the page kept PROT_BTI */
    fputs("branch to a non-landing did not fault: the copy page lost PROT_BTI\n", stderr);
    return 1;
}
