/* Real checkpoint assembly, signal routing, nested replay and post-replay v4
 * publication. Like checkpoint_entry.c, this uses a verifying poison callback,
 * not an ASan runtime; it is not an ASan/full-instrumentation acceptance gate. */
#define _GNU_SOURCE
#undef NDEBUG
#include "checkpoint.h"
#include "fault_sites.h"
#include "signal_handler.h"
#include "runtime_contract.h"
#include "runtime_contract_fingerprint.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

extern const struct teapot_fault_site_table __start_teapot_fault_sites;
extern const char LIBCHECKPOINT_CONTRACT_ANCHOR[];
__attribute__((used,section("teapot_contract"),aligned(8)))
static const struct libcheckpoint_contract_record record={
    .magic=LIBCHECKPOINT_CONTRACT_MAGIC,.version=LIBCHECKPOINT_CONTRACT_VERSION,
    .kind=LIBCHECKPOINT_CONTRACT_KIND_MODULE,.header_size=LIBCHECKPOINT_CONTRACT_HEADER_SIZE,
    .fingerprint=LIBCHECKPOINT_CONTRACT_FINGERPRINT,
    .capabilities=LIBCHECKPOINT_CAPABILITY_NESTED|LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING|
                  LIBCHECKPOINT_CAPABILITY_FAULT_PUBLISHING,
    .anchor=LIBCHECKPOINT_CONTRACT_ANCHOR,.fault_sites=&__start_teapot_fault_sites};
extern memory_history_t *memory_history_top;
extern uintptr_t checkpoint_target_metadata[];
extern uint64_t max_checkpoints;
extern void poison_protected_zone(void);
extern int checkpoint_rollback_probe(void *);
extern void fault_risc_execute(const uint64_t *,uint64_t[14],uint64_t);
extern char __start_teapot_protected[],__stop_teapot_protected[];
extern char __start_teapot_protected_bss[],__stop_teapot_protected_bss[];
static uint32_t branch_counter;
static uint64_t word=0x5555;
static unsigned nested,restart,step,windows_seen,poisoned_ranges,mapped_fault;
static void *fault_page,*access_page,*stack_top;

#ifdef COVERAGE
/* The coverage-enabled archive still requires a module guard range even
 * though this focused fixture does not inject coverage pushes. */
__asm__(".pushsection .data.fault_risc_guards,\"aw\"\n"
        ".balign 4\n.globl __guard_start__teapot__,__guard_end__teapot__\n"
        "__guard_start__teapot__:\n.zero 16\n__guard_end__teapot__:\n.popsection\n");
#endif

#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
extern uint64_t teapot_bti_text_lo,teapot_bti_text_hi,teapot_bti_copy_lo,teapot_bti_copy_hi;
static void enable_fixture_copy_bti(void) {
    /* This is the same manual copy-policy activation as the focused PAC/BTI
     * fixture, not a substitute for complete backend landing activation. */
    const struct teapot_fault_site_table *t=&__start_teapot_fault_sites;
    uintptr_t lo,hi;
    assert(getauxval(AT_HWCAP2)&(1UL<<17));
    assert(teapot_fault_resolve_relative((uintptr_t)&t->text_start,t->text_start,&lo));
    assert(teapot_fault_resolve_relative((uintptr_t)&t->text_end,t->text_end,&hi));
    teapot_bti_text_lo=(uintptr_t)fault_risc_execute;
    teapot_bti_text_hi=teapot_bti_text_lo+4;
    teapot_bti_copy_lo=lo;teapot_bti_copy_hi=hi;
    assert(!mprotect((void *)lo,hi-lo,PROT_READ|PROT_EXEC|0x10));
}
#endif

void __asan_poison_memory_region(void const volatile *address,size_t size) {
    assert(poisoned_ranges<2);
    uintptr_t start=(uintptr_t)(poisoned_ranges?__start_teapot_protected_bss:__start_teapot_protected);
    uintptr_t end=(uintptr_t)(poisoned_ranges?__stop_teapot_protected_bss:__stop_teapot_protected);
    assert((uintptr_t)address==start && size==end-start);++poisoned_ranges;
}
static void logged_write(uint64_t value) {
    *memory_history_top++=(memory_history_t){.addr=&word,.data=word,.size=8};word=value;
}
__attribute__((noreturn)) void checkpoint_test_transient_body(void) {
    if(step++==0) {
        logged_write(0x1111);instruction_cnt+=7;
        if(nested) {
            assert(checkpoint_cnt==1);
            assert(checkpoint_rollback_probe(stack_top?(char *)stack_top-65536:NULL)==1);
            assert(checkpoint_cnt==1 && word==0x1111 && instruction_cnt==24);
        }
    } else {
        assert(nested && checkpoint_cnt==2 && word==0x1111);
        logged_write(0x2222);instruction_cnt+=7;
    }
    memset(dift_reg_tags,0xff,DIFT_REG_TAGS_SIZE);
    memset(dift_reg_queued_tags,0xff,DIFT_REG_TAGS_SIZE);dift_reg_queue_pending[0]=1;
    if(restart) *memory_history_top++=(memory_history_t){.addr=fault_page,.data=UINT64_MAX,.size=8};
    ++windows_seen;
    uint64_t state[14];fault_risc_execute(mapped_fault?access_page:(void *)8,state,0);
    abort();
}
int main(int argc,char **argv) {
    assert(argc==4);nested=atoi(argv[1]);restart=atoi(argv[2]);mapped_fault=atoi(argv[3]);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    fault_page=mmap(NULL,page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    access_page=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(fault_page!=MAP_FAILED && access_page!=MAP_FAILED);
#if defined(__aarch64__)
    size_t size=2*AARCH64_SHADOW_STACK_SIZE;
    void *mapping=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapping!=MAP_FAILED);stack_top=(char *)mapping+size-4096;
#endif
    checkpoint_target_metadata[CHECKPOINT_TARGET_BRANCH_COUNTER_ADDR/8]=(uintptr_t)&branch_counter;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG0_SOURCE/8]=UINTPTR_MAX;
    checkpoint_target_metadata[CHECKPOINT_TARGET_FIXED_REG1_SOURCE/8]=UINTPTR_MAX;
    libcheckpoint_enabled=true;setup_signal_handler();poison_protected_zone();
#ifdef TEAPOT_AARCH64_MTE_TAG_STORAGE
    assert(poisoned_ranges==0); /* MTE intentionally leaves runtime metadata untagged. */
#else
    assert(poisoned_ranges==2);
#endif
#ifdef TEAPOT_EXPERIMENTAL_AARCH64_BTI
    enable_fixture_copy_bti();
#endif
    for(unsigned iteration=0;iteration<4;iteration++) {
        branch_counter=0;max_checkpoints=MAX_CHECKPOINTS;step=0;instruction_cnt=17;
        for(unsigned i=0;i<DIFT_REG_TAGS_SIZE;i++) dift_reg_tags[i]=(dift_tag_t)(i+1);
        assert(checkpoint_cnt==0 && word==0x5555);
        assert(checkpoint_rollback_probe(stack_top)==1);
        assert(checkpoint_cnt==0 && word==0x5555 && !in_restore_memlog && instruction_cnt==17);
        assert(memory_history_top==memory_history && !dift_reg_queue_pending[0]);
        for(unsigned i=0;i<DIFT_REG_TAGS_SIZE;i++) assert(dift_reg_tags[i]==i+1 && !dift_reg_queued_tags[i]);
    }
    unsigned windows=4*(1+nested);
    assert(windows_seen==windows && simulation_statistics.total_ckpt==windows);
    assert(simulation_statistics.rollback_reason[ROLLBACK_SIGSEGV]==windows);
    assert(simulation_statistics.ckpt_depth[0]==4 && simulation_statistics.ckpt_depth[1]==4*nested);
    const struct teapot_fault_risc_entry *e=(const void *)__start_teapot_fault_sites.entries;
    uintptr_t pc;assert(teapot_fault_resolve_relative((uintptr_t)&e->site.fault_pc,e->site.fault_pc,&pc));
    uint32_t current;memcpy(&current,(const void *)pc,4);
    bool enabled=getenv("TEAPOT_FAULT_ADAPTATION")[0]!='0';
    assert((current!=e->original)==enabled);
    assert(teapot_fault_counter(0,0)==(enabled?(mapped_fault?windows:2):0));
    uint64_t second[2]={7,1234567},first[2]={11,(uintptr_t)second},state[14];
    fault_risc_execute(first,state,UINT64_C(0xf0000000));
#if defined(__aarch64__)
    assert(state[0]==second[1] && state[5]==UINT64_C(0xf0000000));
#else
    assert(state[0]==first[0] && state[1]==first[1]);
#endif
    printf("windows=%u depth2=%u replay=%u mapped=%u restored=%lx instruction-count=%lu PASS\n",
           windows,4*nested,restart,mapped_fault,word,instruction_cnt);
}
