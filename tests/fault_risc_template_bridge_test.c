/* The bridge is a real CMake target on every ISA. Python discovery separately
 * compares the full 2592 A64 / 336 RV matrix against its independent encoder. */
#include "fault_sites.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern size_t test_fault_risc_template(const unsigned char *, unsigned, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, unsigned char *, size_t *);

int main(void) {
    for (unsigned isa = 1; isa <= 2; ++isa) {
        for (unsigned dead = 0; dead <= 1; ++dead) {
            struct teapot_fault_risc_entry e = {0};
            e.site.length = 4; e.origin = 1; e.width = 8; e.index = 255;
            e.displacement = 8; e.template_id = dead ? 2 : 1;
            if (isa == 1) {
                e.original = dead ? 0xf9400421 : 0xf9400441;
                e.base = dead ? 1 : 2; e.destination = 1; e.kind = 1; e.spill_size = 24;
                e.bootstrap = dead ? 17 : 1; e.temp0 = 0; e.temp1 = dead ? 2 : 3;
            } else {
                e.original = dead ? 0x0085b583 : 0x00863583;
                e.base = dead ? 11 : 12; e.destination = 11; e.kind = 4; e.spill_size = 16;
                e.bootstrap = dead ? 6 : 11; e.temp0 = 1; e.temp1 = 5;
            }
            unsigned char bytes[128]; size_t copy = 0;
            uintptr_t stub = isa == 1 ? 0x100000 : 0x100002;
            size_t size = test_fault_risc_template((const void *)&e, isa, stub,
                0x204ff8, 0x300800, stub - 500, 0x180000, bytes, &copy);
            assert(size && size <= sizeof(bytes) && copy + 4 < size);
            uint32_t word; memcpy(&word, bytes + copy, sizeof(word));
            assert(word == e.original);
            e.reserved[51] = 1;
            assert(!test_fault_risc_template((const void *)&e, isa, stub,
                0x204ff8, 0x300800, stub - 500, 0x180000, bytes, &copy));
        }
    }
    puts("RISC template bridge, both ISAs and recipes PASS");
}
