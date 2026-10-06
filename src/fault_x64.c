#define _GNU_SOURCE
#include "fault_sites.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

__attribute__((noinline))
long teapot_fault_x64_mprotect(void *address, size_t length, int protection) {
#if defined(__x86_64__)
    long result;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "0"((long)SYS_mprotect), "D"(address), "S"(length), "d"(protection)
                      : "rcx", "r11", "cc", "memory");
    return result;
#else
    (void)address; (void)length; (void)protection;
    return -ENOSYS;
#endif
}

bool teapot_fault_x64_can_publish_addresses(void) {
#if defined(__x86_64__)
    /* Linux ARCH_GET_UNTAG_MASK. Older build headers lack the UAPI name.
     * Raw >=2^56 is not a proof under LAM. If the kernel query is unavailable,
     * hardware without LAM is another exact proof (e.g. this AMD host).
     * Later enabling address masking is unsupported while adaptation is on.
     * The independent enforcing-mode stability rule applies even with
     * adaptation off; see README.md, "Owned mappings and unchanged
     * software-tag stores". */
    uint64_t mask=0;
    if (syscall(SYS_arch_prctl,0x4001,&mask)==0) return mask==UINT64_MAX;
    unsigned eax,ebx,ecx,edx;
    /* Linux X86_FEATURE_LAM: CPUID.7.1:EAX[26]. A CPU without leaf 7 cannot
     * implement it either. Never infer an unmasked mode on a LAM-capable CPU. */
    if (!__get_cpuid_count(7,1,&eax,&ebx,&ecx,&edx)) return true;
    return !(eax & (1u<<26));
#else
    return false; /* This publisher is deliberately x64-only. */
#endif
}

/* This byte vocabulary is mirrored by teapot/fault_x64.py. There is no
 * executable-memory writer here; it reconstructs what startup must see. */
struct builder { unsigned char *out; size_t n, capacity; bool valid; uintptr_t pc; };
static void emit(struct builder *b, const unsigned char *p, size_t n) {
    if (n > b->capacity - b->n) { b->valid = false; return; }
    /* Publication runs on the deliberately poisoned private runtime stack.
     * Never route even an uninstrumented build through ASan's memcpy. */
    volatile unsigned char *out=b->out+b->n;
    for (size_t i=0;i<n;i++) out[i]=p[i];
    b->n += n;
}
#define BYTES(b, ...) do { const unsigned char data[] = {__VA_ARGS__}; emit((b), data, sizeof(data)); } while (0)
static void relative(struct builder *b, uintptr_t target, unsigned offset) {
    int32_t value = 0; uintptr_t next = b->pc + b->n + 4;
    if (!target || target > UINTPTR_MAX - offset || next < b->pc) b->valid = false;
    else {
        target += offset;
        if (target >= next) {
            if (target - next > INT32_MAX) b->valid = false;
            else value = (int32_t)(target - next);
        } else {
            if (next - target > (UINT64_C(1) << 31)) b->valid = false;
            else value = (int32_t)(0u - (uint32_t)(next - target));
        }
    }
    emit(b, (const unsigned char *)&value, 4);
}
#define TARGET(e, f, out) teapot_fault_resolve_relative((uintptr_t)&(e)->f, (e)->f, &(out))

bool teapot_fault_x64_window(const struct teapot_fault_window_entry *e, uintptr_t pc,
                            unsigned char *out, size_t capacity) {
    if (!e || !out || e->site.length < 5 || e->site.length > TEAPOT_FAULT_MAX_WINDOW ||
        capacity < e->site.length) return false;
    volatile unsigned char *destination=out;
    for (size_t i=0;i<e->site.length;i++) destination[i]=e->original[i];
    if (e->rip_offset) {
        uintptr_t target;
        if (e->rip_offset < e->access_length || e->rip_offset + 4 > e->rip_end ||
            e->rip_end > e->site.length || pc > UINTPTR_MAX-e->rip_end ||
            !TARGET(e,rip_target,target)) return false;
        for (size_t i=0;i<4;++i) if (out[e->rip_offset+i]) return false;
        struct builder operand={out,e->rip_offset,capacity,true,pc+e->rip_end-e->rip_offset-4};
        relative(&operand,target,0);
        return operand.valid;
    }
    return !e->rip_target && !e->rip_end;
}

size_t teapot_fault_x64_template(const struct teapot_fault_window_entry *e,
                                unsigned char *out, size_t capacity) {
    uintptr_t stub, copy, ret, spill, low, rollback;
    if (!e || !out || e->base >= 16 || e->base == 4 || e->base == 5 ||
        (e->index != 255 && (e->index >= 16 || e->index == 4 || e->index == 5)) ||
        (e->scale != 1 && e->scale != 2 && e->scale != 4 && e->scale != 8) ||
        (e->index == 255 && e->scale != 1) ||
        (e->address_width != 32 && e->address_width != 64) ||
        e->displacement < INT32_MIN || e->displacement > INT32_MAX ||
        e->site.length < 5 || e->site.length > TEAPOT_FAULT_MAX_WINDOW ||
        !TARGET(e, site.stub, stub) || !TARGET(e, site.copy_pc, copy) ||
        !TARGET(e, return_pc, ret) || !TARGET(e, spill, spill) ||
        !TARGET(e, low, low) || !TARGET(e, rollback, rollback)) return 0;
    struct builder b = {out, 0, capacity, true, stub};
    BYTES(&b, 0x48,0x89,0x05); relative(&b, spill, 0);
    BYTES(&b, 0x4c,0x89,0x1d); relative(&b, spill, 8);
    BYTES(&b, 0x9f,0x0f,0x90,0xc0,0x0f,0xb7,0xc0);
    BYTES(&b, 0x48,0x89,0x05); relative(&b, spill, 16);
    BYTES(&b, 0x48,0x8b,0x05); relative(&b, spill, 0);
    bool sib = e->index != 255 || e->base % 8 == 4;
    unsigned rex = (e->address_width == 64 ? 0x4c : 0x44) | (e->base >> 3);
    if (e->index != 255) rex |= (e->index >> 3) << 1;
    if (e->address_width == 32) BYTES(&b, 0x67);
    BYTES(&b, rex, 0x8d, 0x98 | (sib ? 4 : e->base % 8));
    if (sib) {
        unsigned scale = e->scale == 1 ? 0 : e->scale == 2 ? 1 : e->scale == 4 ? 2 : 3;
        BYTES(&b, (scale << 6) | ((e->index == 255 ? 4 : e->index % 8) << 3) | (e->base % 8));
    }
    int32_t displacement = (int32_t)e->displacement;
    emit(&b, (const unsigned char *)&displacement, 4);
    BYTES(&b, 0x4c,0x3b,0x1d); relative(&b, low, 0);
    BYTES(&b, 0x0f,0x82); size_t low_fail = b.n; BYTES(&b, 0,0,0,0);
    BYTES(&b, 0x49,0xc1,0xeb,0x38);
    BYTES(&b, 0x0f,0x85); size_t high_fail = b.n; BYTES(&b, 0,0,0,0);
    BYTES(&b, 0x48,0x8b,0x05); relative(&b, spill, 16);
    BYTES(&b, 0x04,0x7f,0x9e);
    BYTES(&b, 0x48,0x8b,0x05); relative(&b, spill, 0);
    BYTES(&b, 0x4c,0x8b,0x1d); relative(&b, spill, 8);
    if (stub + b.n != copy) return 0;
    /* A tail RIP operand is bound by startup's semantic-address comparison.
     * Rebuild it from the actual original target, not the template's address. */
    unsigned char window[24];
    if (!teapot_fault_x64_window(e,copy,window,sizeof(window))) return 0;
    emit(&b, window, e->site.length);
    BYTES(&b, 0xe9); relative(&b, ret, 0);
    int32_t to_low_fail = (int32_t)(b.n - low_fail - 4), to_high_fail = (int32_t)(b.n - high_fail - 4);
    if (!b.valid || high_fail + 4 > capacity) return 0;
    for (size_t i=0;i<4;i++) {
        ((volatile unsigned char *)out)[low_fail+i]=((const unsigned char *)&to_low_fail)[i];
        ((volatile unsigned char *)out)[high_fail+i]=((const unsigned char *)&to_high_fail)[i];
    }
    BYTES(&b, 0xe9); relative(&b, rollback, 0);
    return b.valid ? b.n : 0;
}
