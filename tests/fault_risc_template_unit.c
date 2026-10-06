/* Architecture-independent structural/template tests. This does NOT execute
 * generated code or prove native instruction-cache/BTI behavior. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/fault_risc_template.h"

static uint32_t get_word(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static struct teapot_fault_risc_entry fixture(unsigned isa, bool dead) {
    struct teapot_fault_risc_entry e = {0};
    e.site.length = 4; e.origin = 1; e.width = 8; e.index = 255;
    e.displacement = 8; e.template_id = dead ? 2 : 1;
    if (isa == FAULT_RISC_A64) {
        e.original = dead ? 0xf9400421 : 0xf9400441; /* ldr x1,[x1/x2,#8] */
        e.base = dead ? 1 : 2; e.destination = 1; e.kind = 1; e.spill_size = 24;
        e.bootstrap = dead ? 17 : 1; e.temp0 = 0; e.temp1 = dead ? 2 : 3;
    } else {
        e.original = dead ? 0x0085b583 : 0x00863583; /* ld a1,8(a1/a2) */
        e.base = dead ? 11 : 12; e.destination = 11; e.kind = 4; e.spill_size = 16;
        e.bootstrap = dead ? 6 : 11; e.temp0 = 1; e.temp1 = 5;
    }
    return e;
}

static void branch_tests(void) {
    uint32_t encoded;
    for (unsigned isa = FAULT_RISC_A64; isa <= FAULT_RISC_RV64; ++isa) {
        uintptr_t pc = isa == FAULT_RISC_A64 ? 0x10000000 : 0x10000002;
        uintptr_t limit = (uintptr_t)1 << (isa == FAULT_RISC_A64 ? 27 : 20);
        uintptr_t alignment = isa == FAULT_RISC_A64 ? 4 : 2;
        assert(fault_risc_branch(isa, pc, pc - limit, &encoded));
        assert(fault_risc_branch(isa, pc, pc + limit - alignment, &encoded));
        assert(!fault_risc_branch(isa, pc, pc - limit - alignment, &encoded));
        assert(!fault_risc_branch(isa, pc, pc + limit, &encoded));
        assert(!fault_risc_branch(isa, pc, pc + 1, &encoded));
        assert(fault_risc_branch(isa, pc, pc + 4, &encoded));
        assert(encoded == (isa == FAULT_RISC_A64 ? 0x14000001 : 0x0040006f));
    }
    assert(!fault_risc_branch(0, 0x1000, 0x1004, &encoded));
}

static void template_tests(void) {
    for (unsigned isa = FAULT_RISC_A64; isa <= FAULT_RISC_RV64; ++isa) {
        for (unsigned dead = 0; dead < 2; ++dead) {
            struct teapot_fault_risc_entry e = fixture(isa, dead);
            uintptr_t stub = isa == FAULT_RISC_A64 ? 0x100000 : 0x100002;
            unsigned char bytes[128]; size_t copy;
            assert(fault_risc_shape(&e, isa));
            size_t size = fault_risc_template(&e, isa, stub, 0x204ff8, 0x300800,
                                               stub - 500, 0x180000, bytes, sizeof(bytes), &copy);
            assert(size && size % 4 == 0 && copy + 8 < size);
            assert(get_word(bytes + copy) == e.original);
            uint32_t continuation;
            assert(fault_risc_branch(isa, stub + copy + 4, stub - 500, &continuation));
            assert(get_word(bytes + copy + 4) == continuation);
            if (isa == FAULT_RISC_A64) {
                assert(get_word(bytes + copy - 12) == (0xd51b4200 | e.temp0)); /* restore NZCV */
            } else {
                assert(get_word(bytes + size - 4) == 0x00028067); /* jalr zero,t0,0 */
            }
            assert(!fault_risc_template(&e, isa, stub, 0x204ff8, 0x300800,
                                       (uintptr_t)1 << 63, 0x180000, bytes, sizeof(bytes), &copy));
            assert(!fault_risc_template(&e, isa, stub, 0x204ff8, (uintptr_t)1 << 63,
                                       stub - 500, 0x180000, bytes, sizeof(bytes), &copy));
            assert(!fault_risc_template(&e, isa, stub, 0x204ff8, 0x300800,
                                       stub - 500, 0x180000, bytes, 4, &copy));
            struct teapot_fault_risc_entry mutant = e;
            mutant.reserved[51] = 1; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.padding = 1; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.base ^= 1; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.displacement++; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.temp0 = e.bootstrap; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.spill_size = 8; assert(!fault_risc_shape(&mutant, isa));
            mutant = e; mutant.bootstrap = isa == FAULT_RISC_A64 ? 18 : 4;
            assert(!fault_risc_shape(&mutant, isa));
            if (dead) {
                mutant = e; mutant.origin = 2; assert(!fault_risc_shape(&mutant, isa));
                mutant = e; mutant.origin = 3; assert(!fault_risc_shape(&mutant, isa));
            }
        }
    }
}

int main(void) {
    branch_tests(); template_tests();
    puts("RISC template shapes, both recipes, branch boundaries and structural mutants PASS");
    return 0;
}
