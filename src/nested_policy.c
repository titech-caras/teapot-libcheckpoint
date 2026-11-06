#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include "checkpoint.h"
#include "nested_policy.h"

/* Read separately from the program, including /dev/fd/N. Raw syscalls keep
 * this read out of the DIFT libc wrappers and leave the program's offset alone. */
static long seed_syscall(long number, long a, long b, long c, long d) {
#if defined(__x86_64__)
    register long r10 __asm__("r10") = d;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
        : "rcx", "r11", "memory");
    return result;
#elif defined(__aarch64__)
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
#elif defined(__riscv)
    register long a7 __asm__("a7") = number;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    __asm__ volatile("ecall" : "+r"(a0)
        : "r"(a7), "r"(a1), "r"(a2), "r"(a3) : "memory");
    return a0;
#endif
}

void teapot_nested_seed_init(int argc, char **argv) {
    static uint64_t ready LIBCHECKPOINT_PROTECTED_SECTION;
    LIBCHECKPOINT_ASSERT_PROTECTED(ready);
    if (ready)
        return;
    ready = 1;
    const char *override = getenv("TEAPOT_NESTED_SEED");
    if (override) {
        teapot_nested_seed_word = strtoull(override, NULL, 16);
        return;
    }
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ 1;
    if (argc > 1) {
        long fd = seed_syscall(SYS_openat, -100 /* AT_FDCWD */, (long)argv[1], 0, 0);
        if (fd >= 0) {
            unsigned char buffer[1 << 16];
            long n;
            while ((n = seed_syscall(SYS_read, fd, (long)buffer, sizeof buffer, 0)) > 0)
                for (long i = 0; i < n; ++i)
                    h = (h ^ buffer[i]) * UINT64_C(0x100000001b3);
            seed_syscall(SYS_close, fd, 0, 0, 0);
        }
    }
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    teapot_nested_seed_word = h;
}
