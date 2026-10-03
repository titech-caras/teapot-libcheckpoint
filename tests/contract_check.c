/* The start-up contract check: a program linking the runtime with the module
 * record that contract_module_record.c's definitions describe, or with none. */
#include <stdint.h>
#include <stdio.h>

#ifdef COVERAGE
uint32_t contract_test_guard_start[1] __asm__("__guard_start__teapot__");
uint32_t contract_test_guard_end[1] __asm__("__guard_end__teapot__");
#endif

/* Stands for a constructor of the rewritten code: the runtime must refuse a
 * mismatched program before any of them runs. */
__attribute__((constructor)) static void instrumented_constructor(void) {
    fputs("instrumented constructor ran\n", stderr);
}

int main(void) {
    fputs("contract accepted\n", stderr);
    return 0;
}
