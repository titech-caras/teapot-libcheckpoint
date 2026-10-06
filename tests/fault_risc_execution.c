/* Actual v4 registry/publication and executable valid-load checks. The fake
 * rollback target and PC-only test signal handler deliberately do NOT claim
 * to exercise the real nested checkpoint/memlog rollback implementation. */
#define _GNU_SOURCE
#include "fault_sites.h"
#include "runtime_contract.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

extern const struct teapot_fault_site_table __start_teapot_fault_sites;
extern const char __start_teapot_protected_bss[], __stop_teapot_protected_bss[];
extern void fault_risc_execute(const uint64_t *, uint64_t[14], uint64_t);
#ifdef FAULT_RISC_TEST_INTERNAL_MAPS
extern void fault_risc_dump_maps_for_test(const struct teapot_fault_site_table *);
#endif
__attribute__((noreturn)) void restore_checkpoint_SIGSEGV(void) { _exit(88); }
static sigjmp_buf restart;
static volatile sig_atomic_t copied_fault, signals_seen;
static uintptr_t relative(const int32_t *field);

#ifdef FAULT_RISC_TEST_BTI_PAC
#include "config.h"
#define TEST_PROT_BTI 0x10
uint64_t teapot_bti_text_lo LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_bti_text_hi LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_bti_copy_lo LIBCHECKPOINT_PROTECTED_SECTION;
uint64_t teapot_bti_copy_hi LIBCHECKPOINT_PROTECTED_SECTION;
extern void fault_risc_bad_landing(void *);
static uintptr_t bti_target;
static void bti_signal(int sig,siginfo_t *info,void *context) {
    const ucontext_t *uc=context;
    /* This is a branch-target exception, not a copied-load fault. Inspect
     * only PC/PSTATE, never any destination-bootstrap GPR. */
    _exit(sig==SIGILL && info && info->si_code>0 &&
          (uintptr_t)info->si_addr==bti_target && uc->uc_mcontext.pc==bti_target &&
          (uc->uc_mcontext.pstate&(3UL<<10)) ? 0 : 94);
}
static void check_bti(void) {
    pid_t child=fork();assert(child>=0);
    if (!child) {
        struct sigaction action={.sa_sigaction=bti_signal,.sa_flags=SA_SIGINFO};
        sigemptyset(&action.sa_mask);assert(!sigaction(SIGILL,&action,NULL));
        fault_risc_bad_landing((void *)bti_target);_exit(95);
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void enable_test_bti(void) {
    /* This bounded fixture activates only the actual v4 copy mapping. The
     * real combined-backend activation/native gate is a separate obligation. */
    assert((getauxval(AT_HWCAP)&(1UL<<30)) && (getauxval(AT_HWCAP2)&(1UL<<17)));
    const struct teapot_fault_site_table *t=&__start_teapot_fault_sites;
    uintptr_t lo,hi;
    assert(teapot_fault_resolve_relative((uintptr_t)&t->text_start,t->text_start,&lo));
    assert(teapot_fault_resolve_relative((uintptr_t)&t->text_end,t->text_end,&hi));
    teapot_bti_text_lo=(uintptr_t)fault_risc_execute;
    teapot_bti_text_hi=teapot_bti_text_lo+4;
    teapot_bti_copy_lo=lo;teapot_bti_copy_hi=hi;
    assert(!mprotect((void *)lo,hi-lo,PROT_READ|PROT_EXEC|TEST_PROT_BTI));
    bti_target=relative(&((const struct teapot_fault_risc_entry *)t->entries)->site.fault_pc);
    check_bti();
}
#endif

static uintptr_t relative(const int32_t *field) {
    uintptr_t result;
    assert(teapot_fault_resolve_relative((uintptr_t)field,*field,&result));
    return result;
}
static void copied_signal(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc=context;
#if defined(__aarch64__)
    uintptr_t pc=uc->uc_mcontext.pc;
#else
    uintptr_t pc=uc->uc_mcontext.__gregs[0];
#endif
    /* Only the PC is read. In particular a destination-bootstrap copy may
     * hold the private spill address in D: never inspect/print that GPR. */
    if (sig!=SIGSEGV || !info || info->si_code<=0) _exit(90);
    copied_fault=teapot_fault_risc_copied_kernel(pc,true,false);
    teapot_fault_train(sig,info,pc,true,false);
    ++signals_seen;
    siglongjmp(restart,1);
}

static void malformed(void) {
    const struct teapot_fault_site_table *source=&__start_teapot_fault_sites;
    _Alignas(8) static unsigned char buffer[112+2*128];
    assert(source->count==2);
    memcpy(buffer,source,sizeof(buffer));
    struct teapot_fault_site_table *t=(void *)buffer;
    for (unsigned offset=24;offset<88;offset+=8) {
        const int64_t *from=(const void *)((const unsigned char *)source+offset);
        int64_t *to=(void *)(buffer+offset); uintptr_t target;
        assert(teapot_fault_resolve_relative((uintptr_t)from,*from,&target));
        *to=(int64_t)target-(int64_t)(uintptr_t)to;
    }
    const unsigned fields[]={0,4,8,16,20,24,28,32,36,40,44};
    for (unsigned entry=0;entry<2;entry++)
        for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
            unsigned offset=112+entry*128+fields[i];
            const int32_t *from=(const void *)((const unsigned char *)source+offset);
            int32_t *to=(void *)(buffer+offset);
            int64_t delta=(int64_t)relative(from)-(int64_t)(uintptr_t)to;
            assert(delta>=INT32_MIN && delta<=INT32_MAX); *to=(int32_t)delta;
        }
#define VALID() teapot_fault_validate_table(t,sizeof(buffer),(uintptr_t)__start_teapot_protected_bss,(uintptr_t)__stop_teapot_protected_bss)
    assert(VALID());
    struct teapot_fault_risc_entry *e=(void *)t->entries;
    t->reserved[0]=1; assert(!VALID());t->reserved[0]=0;
    t->version=3; assert(!VALID());t->version=4;
    t->entry_size=16; assert(!VALID());t->entry_size=128;
    e[0].site.length=2;assert(!VALID());e[0].site.length=4;
    e[0].reserved[51]=1;assert(!VALID());e[0].reserved[51]=0;
    e[0].width^=1;assert(!VALID());e[0].width^=1;
    e[1].origin=2;assert(!VALID());e[1].origin=1;
    e[0].spill_size=0;assert(!VALID());e[0].spill_size=
#if defined(__aarch64__)
        24;
#else
        16;
#endif
    assert(VALID());
#undef VALID
}

static void valid_loads(bool published) {
    uint64_t second[2]={0x777,0x123456789abcdef0ULL};
    uint64_t first[2]={0x222,(uintptr_t)second};
    uint64_t state[14];
    for (unsigned flags=0;flags<16;flags++) {
        memset(state,0,sizeof(state));
        fault_risc_execute(first,state,(uint64_t)flags<<28);
#if defined(__aarch64__)
        assert(state[0]==second[1]);
        assert(state[1]==(uintptr_t)first && state[2]==(uintptr_t)first && state[3]==0x333);
        assert(state[5]==(uint64_t)flags<<28);
        if (!published) assert(state[4]==0x777);
#else
        assert(state[0]==first[0] && state[1]==first[1] && state[2]==0x555 && state[4]==(uintptr_t)first);
        if (!published) assert(state[3]==0x666);
        assert(state[12]==state[13]); /* fp */
#endif
        assert(state[6]==state[7]);  /* sp */
        assert(state[8]==state[9]);  /* x18 / gp */
        assert(state[10]==state[11]);/* fp / tp */
    }
}

static void impossible_copy(uintptr_t copy, bool replay) {
    pid_t child=fork();assert(child>=0);
    if (!child) { teapot_fault_risc_copied_kernel(copy,replay,replay); _exit(91); }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==127);
}

int main(int argc,char **argv) {
    assert(argc==2);bool enabled=!strcmp(argv[1],"on");
    assert(!setenv("TEAPOT_FAULT_ADAPTATION",enabled?"1":"0",1));
#ifdef FAULT_RISC_TEST_MAPS
    /* Diagnostic-only startup context, before any guard can change D. */
    int fd=open("/proc/self/maps",O_RDONLY);assert(fd>=0);
    char mapping[1024];ssize_t count;
    while ((count=read(fd,mapping,sizeof(mapping)))>0) assert(write(2,mapping,(size_t)count)==count);
    assert(count==0 && !close(fd));
    for (unsigned offset=24;offset<88;offset+=8) {
        const int64_t *field=(const void *)((const unsigned char *)&__start_teapot_fault_sites+offset);
        uintptr_t address;assert(teapot_fault_resolve_relative((uintptr_t)field,*field,&address));
        fprintf(stderr,"table[%u]=%lx\n",offset,(unsigned long)address);
    }
#endif
    malformed();
#ifdef FAULT_RISC_TEST_INTERNAL_MAPS
    fault_risc_dump_maps_for_test(&__start_teapot_fault_sites);
#endif
    const struct libcheckpoint_contract_record record={
        .magic=LIBCHECKPOINT_CONTRACT_MAGIC,.version=LIBCHECKPOINT_CONTRACT_VERSION,
        .kind=LIBCHECKPOINT_CONTRACT_KIND_MODULE,.header_size=sizeof(record),
        .capabilities=LIBCHECKPOINT_CAPABILITY_FAULT_TRAINING|LIBCHECKPOINT_CAPABILITY_FAULT_PUBLISHING,
        .fault_sites=&__start_teapot_fault_sites};
    teapot_fault_registry_initialize(&record,(const char *)&record+sizeof(record));
    const struct teapot_fault_risc_entry *entries=(const void *)__start_teapot_fault_sites.entries;
#ifdef FAULT_RISC_TEST_BTI_PAC
    enable_test_bti();
#endif
    valid_loads(false);
    teapot_fault_publish_pending(); /* empty ring leaves original words alone */
    siginfo_t info={.si_code=SEGV_MAPERR};
    for (unsigned site=0;site<2;site++) {
        const struct teapot_fault_risc_entry *e=&entries[site];
        uintptr_t pc=relative(&e->site.fault_pc),copy=relative(&e->site.copy_pc);
        uint32_t word;memcpy(&word,(void *)pc,4);assert(word==e->original);
        assert(!teapot_fault_risc_copied_kernel(pc,true,false));
        assert(teapot_fault_risc_copied_kernel(copy,true,false));
        impossible_copy(copy,false);impossible_copy(copy,true);
        for (unsigned n=0;n<2;n++) assert(teapot_fault_train(SIGSEGV,&info,pc,true,false)==enabled);
    }
    teapot_fault_publish_pending(); /* actual raw publisher, not a mock */
    for (unsigned site=0;site<2;site++) {
        uint32_t word;memcpy(&word,(void *)relative(&entries[site].site.fault_pc),4);
        assert((word!=entries[site].original)==enabled);
        assert(teapot_fault_counter(0,site)==(enabled?2:0));
    }
    valid_loads(enabled); /* valid address still executes exact loads after training */
#ifdef FAULT_RISC_TEST_BTI_PAC
    check_bti(); /* same page and nonlanding target, after actual publication */
#endif
    struct sigaction action={.sa_sigaction=copied_signal,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);assert(!sigaction(SIGSEGV,&action,NULL));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    void *inaccessible=mmap(NULL,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(inaccessible!=MAP_FAILED);
    if (!sigsetjmp(restart,1)) {
        uint64_t ignored[14];fault_risc_execute(inaccessible,ignored,0);assert(0);
    }
    assert(signals_seen==1 && copied_fault==enabled);
    assert(teapot_fault_counter(0,0)==(enabled?3:0));
    if (enabled) {
        assert(teapot_fault_risc_policy.low>=page);
        pid_t child=fork();assert(child>=0);
        if (!child) { uint64_t ignored[14];fault_risc_execute((void *)8,ignored,0);_exit(92); }
        int status;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==88);
#if defined(__aarch64__)
        child=fork();assert(child>=0);
        if (!child) { uint64_t ignored[14];fault_risc_execute((void *)UINT64_C(0xab00000000000008),ignored,0);_exit(93); }
        assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==88);
#endif
    } else assert(!teapot_fault_risc_policy.low && !teapot_fault_risc_policy.high);
    printf("v4 %s: valid loads/registers/flags; published=%u; kernel-copy=%u; malformed/impossible contexts PASS\n",
           enabled?"on":"off",enabled,copied_fault);
    return 0;
}
