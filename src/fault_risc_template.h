/* Pure v4 decoder/template reconstruction; no allocation, assembly callbacks,
 * application-stack access or global scratch. Kept private to fault_sites.c.
 */
#ifndef TEAPOT_PRIVATE_FAULT_RISC_TEMPLATE_H
#define TEAPOT_PRIVATE_FAULT_RISC_TEMPLATE_H
#include "fault_sites.h"

enum fault_risc_isa { FAULT_RISC_A64 = 1, FAULT_RISC_RV64 = 2 };

static bool fault_risc_register(unsigned isa, unsigned reg) {
    if (isa == FAULT_RISC_A64) return reg < 31 && reg != 18 && reg != 29;
    return isa == FAULT_RISC_RV64 && reg > 0 && reg < 32 && reg != 2 && reg != 3 && reg != 4 && reg != 8;
}

static int64_t fault_risc_signed(uint32_t value, unsigned bits) {
    int64_t result = value;
    if (value & (UINT32_C(1) << (bits - 1))) result -= INT64_C(1) << bits;
    return result;
}

static bool fault_risc_delta(uintptr_t source, uintptr_t target, unsigned bits, int64_t *out) {
    uint64_t limit = UINT64_C(1) << (bits - 1);
    if (target >= source) {
        if (target - source >= limit) return false;
        *out = (int64_t)(target - source);
    } else {
        if (source - target > limit) return false;
        *out = -(int64_t)(source - target);
    }
    return true;
}

static bool fault_risc_branch(unsigned isa, uintptr_t pc, uintptr_t target, uint32_t *word) {
    int64_t delta;
    if (isa == FAULT_RISC_A64) {
        if ((pc | target) & 3 || !fault_risc_delta(pc, target, 28, &delta)) return false;
        *word = UINT32_C(0x14000000) | ((uint64_t)delta >> 2 & UINT32_C(0x3ffffff));
        return true;
    }
    if (isa != FAULT_RISC_RV64 || (pc | target) & 1 || !fault_risc_delta(pc, target, 21, &delta)) return false;
    uint32_t d = (uint32_t)delta;
    *word = 0x6f | ((d >> 20) & 1) << 31 | ((d >> 1) & 1023) << 21 |
            ((d >> 11) & 1) << 20 | ((d >> 12) & 255) << 12;
    return true;
}

static bool fault_risc_shape(const struct teapot_fault_risc_entry *e, unsigned isa) {
    uint32_t w = e->original;
    unsigned width, base, destination, index = 255, extension = 0, shift = 0, kind;
    int64_t displacement = 0;
    if (isa == FAULT_RISC_A64) {
        unsigned size = w >> 30, opc = w >> 22 & 3;
        if (w & (1u << 26) || !opc || (opc == 2 && size == 3) || (opc == 3 && size >= 2)) return false;
        width = 1u << size; base = w >> 5 & 31; destination = w & 31;
        if ((w & UINT32_C(0x3b000000)) == UINT32_C(0x39000000)) {
            kind = 1; displacement = (w >> 10 & 4095) << size;
        } else if ((w & UINT32_C(0x3b200c00)) == UINT32_C(0x38000000)) {
            kind = 2; displacement = fault_risc_signed(w >> 12 & 511, 9);
        } else if ((w & UINT32_C(0x3b200c00)) == UINT32_C(0x38200800)) {
            kind = 3; index = w >> 16 & 31; extension = w >> 13 & 7;
            shift = w & 4096 ? size : 0;
            if (extension != 2 && extension != 3 && extension != 6 && extension != 7) return false;
        } else return false;
    } else if (isa == FAULT_RISC_RV64) {
        unsigned funct = w >> 12 & 7;
        if ((w & 127) != 3 || funct == 7) return false;
        width = 1u << (funct & 3); base = w >> 15 & 31; destination = w >> 7 & 31;
        displacement = fault_risc_signed(w >> 20, 12); kind = 4;
    } else return false;
    if (!fault_risc_register(isa, base) || !fault_risc_register(isa, destination) ||
        (index != 255 && !fault_risc_register(isa, index)) ||
        width != e->width || base != e->base || destination != e->destination || index != e->index ||
        extension != e->extension || shift != e->shift || kind != e->kind || displacement != e->displacement ||
        e->padding || e->origin < 1 || e->origin > 3 || e->spill_size != (isa == FAULT_RISC_A64 ? 24 : 16)) return false;
    if (e->template_id == 1) {
        if (e->bootstrap != destination || destination == base || destination == index) return false;
    } else if (e->template_id == 2) {
        if (e->origin != 1 || (destination != base && destination != index) ||
            !fault_risc_register(isa, e->bootstrap) || e->bootstrap == destination ||
            e->bootstrap == base || e->bootstrap == index) return false;
    } else return false;
    unsigned found = 0, temporaries[2] = {255, 255};
    for (unsigned reg = 0; reg < 32 && found < 2; ++reg)
        if (fault_risc_register(isa, reg) && reg != base && reg != index &&
                reg != destination && reg != e->bootstrap) temporaries[found++] = reg;
    if (found != 2 || e->temp0 != temporaries[0] || e->temp1 != temporaries[1]) return false;
    for (size_t i = 0; i < sizeof(e->reserved); ++i) if (e->reserved[i]) return false;
    return true;
}

struct fault_risc_writer { uint32_t words[32]; size_t count; bool valid; };
static size_t fault_risc_word(struct fault_risc_writer *w, uint32_t word) {
    if (w->count == sizeof(w->words) / sizeof(w->words[0])) { w->valid = false; return 0; }
    size_t offset = w->count++ * 4;
    w->words[offset / 4] = word;
    return offset;
}

static void fault_risc_address(struct fault_risc_writer *w, unsigned isa, unsigned reg,
                               uintptr_t start, uintptr_t target, uint32_t middle) {
    uintptr_t pc = start + w->count * 4;
    int64_t delta;
    if (isa == FAULT_RISC_A64) {
        if (!fault_risc_delta(pc >> 12, target >> 12, 21, &delta)) { w->valid = false; return; }
        uint32_t d = (uint32_t)delta;
        fault_risc_word(w, UINT32_C(0x90000000) | (d & 3) << 29 | ((d >> 2) & 0x7ffff) << 5 | reg);
        if (middle) fault_risc_word(w, middle);
        fault_risc_word(w, UINT32_C(0x91000000) | (target & 4095) << 10 | reg << 5 | reg);
    } else {
        if (middle) { w->valid = false; return; }
        if (!fault_risc_delta(pc, target, 33, &delta)) { w->valid = false; return; }
        int64_t rounded = delta + 2048;
        int64_t high = rounded >= 0 ? rounded / 4096 : -((-rounded + 4095) / 4096);
        if (high < -(INT64_C(1) << 19) || high >= (INT64_C(1) << 19)) { w->valid = false; return; }
        fault_risc_word(w, ((uint32_t)high & 0xfffff) << 12 | reg << 7 | 0x17);
        fault_risc_word(w, ((uint32_t)delta & 4095) << 20 | reg << 15 | reg << 7 | 0x13);
    }
}

static uint32_t fault_risc_rv_i(unsigned op, unsigned rd, unsigned rs, int64_t immediate) {
    return op | rd << 7 | rs << 15 | ((uint32_t)immediate & 4095) << 20;
}
static uint32_t fault_risc_rv_store(unsigned base, unsigned value, unsigned offset) {
    return 0x3023 | (offset & 31) << 7 | base << 15 | value << 20 | (offset >> 5) << 25;
}
static uint32_t fault_risc_rv_cond(unsigned a, unsigned b, unsigned delta, unsigned funct) {
    return 0x63 | funct << 12 | a << 15 | b << 20 | ((delta >> 11) & 1) << 7 |
           ((delta >> 1) & 15) << 8 | ((delta >> 5) & 63) << 25 | ((delta >> 12) & 1) << 31;
}

static size_t fault_risc_template(const struct teapot_fault_risc_entry *e, unsigned isa,
        uintptr_t stub, uintptr_t spill, uintptr_t policy, uintptr_t continuation, uintptr_t rollback,
        unsigned char *output, size_t capacity, size_t *copy_offset) {
    if (!e || !output || !copy_offset || !fault_risc_shape(e, isa) ||
            stub > UINTPTR_MAX - 128 || (stub & (isa == FAULT_RISC_A64 ? 3u : 1u))) return 0;
    struct fault_risc_writer w = {.count = 0, .valid = true};
    unsigned b = e->bootstrap, t0 = e->temp0, t1 = e->temp1;
    size_t low, high = 0;
    fault_risc_address(&w, isa, b, stub, spill, 0);
    if (isa == FAULT_RISC_A64) {
        fault_risc_word(&w, UINT32_C(0xf9000000) | b << 5 | t0);
        fault_risc_word(&w, UINT32_C(0xf9000400) | b << 5 | t1);
        fault_risc_word(&w, UINT32_C(0xd53b4200) | t0);
        fault_risc_word(&w, UINT32_C(0xf9000800) | b << 5 | t0);
        if (e->index != 255)
            fault_risc_word(&w, UINT32_C(0x8b200000) | e->index << 16 | e->extension << 13 |
                e->shift << 10 | e->base << 5 | t0);
        else {
            uint32_t imm = e->displacement < 0 ? (uint32_t)-e->displacement : (uint32_t)e->displacement;
            uint32_t op = e->displacement < 0 ? UINT32_C(0xd1000000) : UINT32_C(0x91000000);
            fault_risc_word(&w, op | (imm & 4095) << 10 | e->base << 5 | t0);
            if (imm >> 12) fault_risc_word(&w, op | 1u << 22 | (imm >> 12) << 10 | t0 << 5 | t0);
        }
        /* Independent ordinary data-TBI normalization prevents adjacency-based
         * ADRP/ADD relaxation without adding an instruction or changing NZCV. */
        fault_risc_address(&w, isa, t1, stub, policy,
            UINT32_C(0xd3400000) | 55u << 10 | t0 << 5 | t0);
        fault_risc_word(&w, UINT32_C(0xf9400000) | t1 << 5 | t1);
        fault_risc_word(&w, UINT32_C(0xeb00001f) | t1 << 16 | t0 << 5);
        low = fault_risc_word(&w, 0);
        fault_risc_word(&w, UINT32_C(0xf9400800) | b << 5 | t0);
        fault_risc_word(&w, UINT32_C(0xd51b4200) | t0);
        fault_risc_word(&w, UINT32_C(0xf9400000) | b << 5 | t0);
        fault_risc_word(&w, UINT32_C(0xf9400400) | b << 5 | t1);
    } else {
        fault_risc_word(&w, fault_risc_rv_store(b, t0, 0));
        fault_risc_word(&w, fault_risc_rv_store(b, t1, 8));
        fault_risc_word(&w, fault_risc_rv_i(0x13, t0, e->base, e->displacement));
        fault_risc_address(&w, isa, t1, stub, policy, 0);
        fault_risc_word(&w, fault_risc_rv_i(0x3003, t1, t1, 0));
        low = fault_risc_word(&w, 0);
        fault_risc_address(&w, isa, t1, stub, policy, 0);
        fault_risc_word(&w, fault_risc_rv_i(0x3003, t1, t1, 8));
        size_t disabled = fault_risc_word(&w, 0);
        high = fault_risc_word(&w, 0);
        w.words[disabled / 4] = fault_risc_rv_cond(t1, 0, w.count * 4 - disabled, 0);
        fault_risc_word(&w, fault_risc_rv_i(0x3003, t0, b, 0));
        fault_risc_word(&w, fault_risc_rv_i(0x3003, t1, b, 8));
    }
    *copy_offset = fault_risc_word(&w, e->original);
    uint32_t jump;
    if (!fault_risc_branch(isa, stub + w.count * 4, continuation, &jump)) return 0;
    fault_risc_word(&w, jump);
    size_t fail = w.count * 4;
    if (isa == FAULT_RISC_A64) {
        w.words[low / 4] = UINT32_C(0x54000003) | (uint32_t)((fail - low) / 4) << 5;
        if (!fault_risc_branch(isa, stub + fail, rollback, &jump)) return 0;
        fault_risc_word(&w, jump);
    } else {
        w.words[low / 4] = fault_risc_rv_cond(t0, t1, fail - low, 6);
        w.words[high / 4] = fault_risc_rv_cond(t0, t1, fail - high, 7);
        fault_risc_address(&w, isa, 5, stub, rollback, 0); /* architectural t0, fail-only */
        fault_risc_word(&w, fault_risc_rv_i(0x67, 0, 5, 0));
    }
    if (!w.valid || w.count * 4 > capacity) return 0;
    for (size_t i = 0; i < w.count; ++i)
        for (unsigned byte = 0; byte < 4; ++byte) output[i * 4 + byte] = (unsigned char)(w.words[i] >> (byte * 8));
    return w.count * 4;
}
#endif
