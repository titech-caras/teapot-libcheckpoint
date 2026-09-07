#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

extern void make_report_call_nop(uint64_t address);

/* Exercise the runtime patcher directly, without checkpoint initialization. */
static int check_call(int got, int expanded) {
    uint32_t *code = mmap(NULL, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    code[0] = 0xa9bf7bfd; /* stp x29, x30, [sp, #-16]! */
    code[1] = 0x910003fd; /* mov x29, sp */
    code[2] = 0x528000a0; /* mov w0, #5 */
    code[3] = 0xaa1f03f0; /* mov x16, xzr: expose a missing address setup */
    code[4] = 0x90000010; /* adrp x16, this page */
    code[5] = got ? 0xf9408210 : 0x91040210; /* ldr [x16,#256] / add #256 */
    code[6] = 0xd63f0200; /* blr x16 */
    code[7] = 0xa8c17bfd; /* ldp x29, x30, [sp], #16 */
    code[8] = 0xd65f03c0; /* ret */
    uint32_t *callee = code + (got ? 68 : 64);
    callee[0] = 0x11000400; /* add w0, w0, #1 */
    callee[1] = 0xd65f03c0; /* ret */
    if (got) {
        uintptr_t target = (uintptr_t)callee;
        memcpy(code + 64, &target, sizeof(target));
    }
    if (!expanded) {
        code[4] = code[5] = 0xd503201f;
        code[6] = 0x94000000 | (uint32_t)(callee - (code + 6)); /* bl */
    }
    uint32_t setup[2] = {code[4], code[5]};
    __builtin___clear_cache((char *)code, (char *)(code + 70));
    int (*call)(void) = (int (*)(void))code;
    if (call() != 6) { fputs("initial call failed\n", stderr); return 1; }
    make_report_call_nop((uintptr_t)(code + (expanded == 1 ? 4 : 6)));
    if (code[4] != setup[0] || code[5] != setup[1] || code[6] != 0xd503201f) {
        fputs("report suppression patched address setup instead of the call\n", stderr);
        return 1;
    }
    if (call() != 5 || call() != 5) {
        fputs("suppressed call did not preserve fallthrough\n", stderr);
        return 1;
    }
    make_report_call_nop((uintptr_t)(code + (expanded == 1 ? 4 : 6)));
    if (code[4] != setup[0] || code[5] != setup[1] || call() != 5) {
        fputs("repeated suppression changed address setup\n", stderr);
        return 1;
    }
    munmap(code, 8192);
    return 0;
}

int main(void) {
    if (check_call(0, 0) | check_call(0, 1) | check_call(1, 1) | check_call(1, 2))
        return 1;
    puts("AArch64 short and expanded report-call suppression: 4/4 passed");
    return 0;
}
