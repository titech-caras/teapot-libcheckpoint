/* Test-only host bridge for byte-for-byte Python/C reconstruction parity.
 * This is never a runtime archive source or generated application helper. */
#include <string.h>
#include "../src/fault_risc_template.h"

size_t test_fault_risc_template(const unsigned char *record, unsigned isa,
        uintptr_t stub, uintptr_t spill, uintptr_t policy, uintptr_t continuation,
        uintptr_t rollback, unsigned char *output, size_t *copy_offset) {
    struct teapot_fault_risc_entry e;
    memcpy(&e, record, sizeof(e));
    return fault_risc_template(&e, isa, stub, spill, policy, continuation, rollback,
                               output, 128, copy_offset);
}
