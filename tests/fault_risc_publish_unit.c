/* Host-only fault injection: tests transaction ordering, not native I-cache or
 * BTI enforcement. Production helpers are direct raw syscalls/cache operations.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static long fault_risc_protect(uintptr_t, uintptr_t, int);
static bool fault_risc_cache_sync(uintptr_t);
static void fault_risc_after_rx(void);
#define FAULT_RISC_PUBLISH_TEST
#include "../src/fault_risc_publish.h"

static unsigned char code[8192] __attribute__((aligned(4096)));
static unsigned mprotect_calls, cache_calls, after_rx_calls;
static uint64_t failed_mprotect, failed_cache;
static int page_protections[2], expected_protections[2];
static int seen_protections[32];
static uintptr_t patch_pc;
static const uint32_t before_word = 0x00053583, after_word = 0x0040006f;

static uint32_t word(void) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= (uint32_t)((unsigned char *)patch_pc)[i] << (8 * i);
    return result;
}

static long fault_risc_protect(uintptr_t page, uintptr_t size, int protection) {
    assert(size == 4096 && page >= (uintptr_t)code && page < (uintptr_t)code + sizeof(code));
    assert(!((protection & PROT_WRITE) && (protection & PROT_EXEC)));
    size_t index = (page - (uintptr_t)code) / 4096;
    /* BTI may not disappear on a RW transition or an RX restoration. */
    assert((protection & PROT_BTI) == (expected_protections[index] & PROT_BTI));
    assert(mprotect_calls < 32);
    seen_protections[mprotect_calls++] = protection;
    if (failed_mprotect & (UINT64_C(1) << (mprotect_calls - 1))) return -1;
    page_protections[index] = protection;
    return 0;
}

static bool fault_risc_cache_sync(uintptr_t pc) {
    assert(pc == patch_pc);
    size_t count = 1 + ((pc + 3) / 4096 - pc / 4096);
    for (size_t i = 0; i < count; ++i) {
        assert(page_protections[i] & PROT_WRITE);
        assert(!(page_protections[i] & PROT_EXEC));
    }
    ++cache_calls;
    assert(word() == (cache_calls == 1 ? after_word : before_word));
    return !(failed_cache & (UINT64_C(1) << (cache_calls - 1)));
}

static void fault_risc_after_rx(void) { ++after_rx_calls; }

static enum fault_risc_patch_result run(bool crossing, int extra,
                                        uint64_t protect_failures, uint64_t cache_failures) {
    patch_pc = (uintptr_t)code + (crossing ? 4094 : 2);
    for (unsigned i = 0; i < 4; ++i) ((unsigned char *)patch_pc)[i] = before_word >> (8 * i);
    mprotect_calls = cache_calls = after_rx_calls = 0;
    memset(seen_protections, 0, sizeof(seen_protections));
    failed_mprotect = protect_failures;
    failed_cache = cache_failures;
    struct fault_risc_page pages[2];
    for (size_t i = 0; i < 2; ++i) {
        page_protections[i] = expected_protections[i] = PROT_READ | PROT_EXEC | extra;
        pages[i] = (struct fault_risc_page){(uintptr_t)code + 4096 * i, page_protections[i]};
    }
    return fault_risc_publish_word(patch_pc, before_word, after_word, 4096, pages, crossing ? 2 : 1);
}

static void policy_tests(void) {
    int protection = 0;
    assert(fault_risc_page_policy(0x20000, 4096, PROT_READ | PROT_EXEC, 0, 0, 0, 0, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    assert(fault_risc_page_policy(0x20000, 4096, PROT_READ | PROT_EXEC,
                                 0x10000, 0x11000, 0x20000, 0x30000, &protection));
    assert(protection == (PROT_READ | PROT_EXEC | PROT_BTI));
    assert(!fault_risc_page_policy(0x10000, 4096, PROT_READ | PROT_EXEC,
                                  0x10000, 0x11000, 0x20000, 0x30000, &protection));
    assert(!fault_risc_page_policy(0x20000, 4096, PROT_READ | PROT_EXEC,
                                  0, 0, 0x20000, 0x30000, &protection));
    assert(!fault_risc_page_policy(0x20000, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  0, 0, 0, 0, &protection));
    assert(!fault_risc_page_policy(0x20000, 65537, PROT_READ | PROT_EXEC, 0, 0, 0, 0, &protection));
    assert(!fault_risc_page_policy(UINTPTR_MAX - 4095, 4096, PROT_READ | PROT_EXEC,
                                  0, 0, 0, 0, &protection));
}

int main(void) {
    policy_tests();
    for (unsigned bti = 0; bti < 2; ++bti) {
        int extra = bti ? PROT_BTI : 0;
        for (unsigned crossing = 0; crossing < 2; ++crossing) {
            assert(run(crossing, extra, 0, 0) == FAULT_RISC_PATCH_PUBLISHED);
            assert(word() == after_word && cache_calls == 1 && after_rx_calls == 1);
            assert(mprotect_calls == (crossing ? 4 : 2));
            assert(run(crossing, extra, 1, 0) == FAULT_RISC_PATCH_REFUSED);
            assert(word() == before_word && cache_calls == 0);
            assert(run(crossing, extra, 0, 1) == FAULT_RISC_PATCH_REFUSED);
            assert(word() == before_word && cache_calls == 2);
            assert(run(crossing, extra, 0, 3) == FAULT_RISC_PATCH_UNSAFE);
            assert(word() == before_word && cache_calls == 2);
        }
        /* Second RW refused: first page restored, no bytes/cache operation. */
        assert(run(true, extra, 1u << 1, 0) == FAULT_RISC_PATCH_REFUSED);
        assert(word() == before_word && cache_calls == 0 && mprotect_calls == 3);
        /* Second RX fails after first RX succeeded: reacquire RW on BOTH. */
        assert(run(true, extra, 1u << 3, 0) == FAULT_RISC_PATCH_REFUSED);
        assert(word() == before_word && cache_calls == 2 && mprotect_calls == 8);
        assert(seen_protections[4] == (PROT_READ | PROT_WRITE | extra));
        assert(seen_protections[5] == (PROT_READ | PROT_WRITE | extra));
        /* Reacquiring RW for undo fails: fail without writing into RX. */
        assert(run(true, extra, (1u << 3) | (1u << 4), 0) == FAULT_RISC_PATCH_UNSAFE);
        assert(word() == after_word && cache_calls == 1);
        /* Undo RX fails: no safe application continuation is claimed. */
        assert(run(false, extra, (1u << 1) | (1u << 3), 0) == FAULT_RISC_PATCH_UNSAFE);
        assert(word() == before_word && cache_calls == 2);
        /* Partial RW refusal with failed restoration also cannot resume. */
        assert(run(true, extra, (1u << 1) | (1u << 2), 0) == FAULT_RISC_PATCH_UNSAFE);
        assert(word() == before_word && cache_calls == 0);
    }
    puts("RISC publisher transaction: policy, BTI attributes, halfword/cross-page, failure/undo PASS");
    return 0;
}
