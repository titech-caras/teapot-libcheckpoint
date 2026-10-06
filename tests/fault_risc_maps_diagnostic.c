/* Diagnostic-only translation unit. Inspect the unmodified raw mapping
 * parser before registry initialization; never include in a runtime archive. */
#define ENABLE_FAULT_TRAINING
#define ENABLE_FAULT_PUBLISHING
#include "../src/fault_sites.c"

void fault_risc_dump_maps_for_test(const struct teapot_fault_site_table *t) {
    size_t count;
    if (!read_mappings(&count)) abort();
    for (size_t i=0;i<count;i++)
        fprintf(stderr,"raw[%zu]=%lx-%lx %s\n",i,(unsigned long)startup_maps[i].start,
                (unsigned long)startup_maps[i].end,startup_maps[i].permissions);
    const int64_t *fields=&t->text_start;
    for (unsigned i=0;i<8;i+=2) {
        uintptr_t start,end;
        if (!teapot_fault_resolve_relative((uintptr_t)&fields[i],fields[i],&start) ||
            !teapot_fault_resolve_relative((uintptr_t)&fields[i+1],fields[i+1],&end)) abort();
        fprintf(stderr,"raw-range[%u]=%lx-%lx mapped=%u\n",i,(unsigned long)start,
                (unsigned long)end,mapped(startup_maps,count,start,end,i>=4,i<4));
    }
}
