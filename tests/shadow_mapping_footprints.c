#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#undef NDEBUG
/* White-box query fault injection, not an alternative production provider.
 * The live wrapper test separately exercises actual kernel metadata. */
#if defined(__x86_64__)
#include <cpuid.h>
static int footprint_cpuid(unsigned,unsigned,unsigned *,unsigned *,unsigned *,unsigned *);
#define __get_cpuid_count footprint_cpuid
#endif
static int footprint_getpagesize(void);
#define getpagesize footprint_getpagesize
#define syscall footprint_syscall
#include "../src/shadow_mapping.c"
#undef syscall
#undef getpagesize
#if defined(__x86_64__)
#undef __get_cpuid_count
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
extern long syscall(long, ...);

enum { MEMINFO_FD = 10001, SMAPS_FD = 10002 };
static const char *meminfo_text, *smaps_text;
static size_t meminfo_offset, smaps_offset, query_chunk;
static int query_error, statfs_error, actual_statfs, raise_after_query;
static uintptr_t statfs_type, statfs_page, untag_mask;
static long tagged_mode;
static int cpuid_lam;
static int simulated_page = 4096;
static unsigned address_queries, mask_queries, open_queries, close_queries, checks;
static int result_errno, entry_errno;
static void *mapping_result;
static int use_real_mmap, replacement_fd = -1, replaced_fd = -1;
static volatile sig_atomic_t delivered;
static int *mapping_calls;
static const char *normal_smaps =
    "1000-2000 rw-p 00000000 00:00 0\nKernelPageSize: 4 kB\nVmFlags: rd wr\n"
    "400000-600000 rw-p 00000000 00:00 0\nKernelPageSize: 2048 kB\nVmFlags: rd wr\n";
static const char *huge_smaps =
    "400000-600000 rw-p 00000000 00:00 0\nKernelPageSize: 2048 kB\nVmFlags: rd ht wr\n";

static int footprint_getpagesize(void) { return simulated_page; }

#if defined(__x86_64__)
static int footprint_cpuid(unsigned leaf,unsigned subleaf,unsigned *eax,
                           unsigned *ebx,unsigned *ecx,unsigned *edx) {
    assert(leaf==7 && subleaf==1);
    *eax=cpuid_lam ? 1U<<26 : 0;
    *ebx=*ecx=*edx=0;
    return 1;
}
#endif

static void deliver(int signal_number) {
    assert(signal_number == SIGUSR1);
    ++delivered;
    if (replaced_fd >= 0) assert(dup2(replacement_fd,replaced_fd) == replaced_fd);
    errno = EDOM;
}

long footprint_syscall(long number, ...) {
    va_list arguments;
    va_start(arguments,number);
    long result = -1;
    if (number == SYS_rt_sigprocmask) {
        int how = va_arg(arguments,int);
        void *mask = va_arg(arguments,void *), *previous = va_arg(arguments,void *);
        size_t length = va_arg(arguments,size_t);
        ++mask_queries;
        result = syscall(number,how,mask,previous,length);
    } else if (number == SYS_write) {
        int fd = va_arg(arguments,int);
        void *buffer = va_arg(arguments,void *);
        size_t length = va_arg(arguments,size_t);
        result = syscall(number,fd,buffer,length);
    } else if (number == SYS_openat) {
        (void)va_arg(arguments,int);
        const char *path = va_arg(arguments,const char *);
        (void)va_arg(arguments,int);
        (void)va_arg(arguments,int);
        ++open_queries;
        if (query_error) errno = query_error;
        else if (!strcmp(path,"/proc/meminfo") && meminfo_text) {
            meminfo_offset = 0; result = MEMINFO_FD;
        } else if (!strcmp(path,"/proc/self/smaps") && smaps_text) {
            smaps_offset = 0; result = SMAPS_FD;
        } else errno = ENOENT;
        if (raise_after_query) { raise_after_query = 0; assert(!raise(SIGUSR1)); }
    } else if (number == SYS_read) {
        int fd = va_arg(arguments,int);
        char *destination = va_arg(arguments,char *);
        size_t capacity = va_arg(arguments,size_t);
        const char *source = fd == MEMINFO_FD ? meminfo_text : smaps_text;
        size_t *offset = fd == MEMINFO_FD ? &meminfo_offset : &smaps_offset;
        assert(fd == MEMINFO_FD || fd == SMAPS_FD);
        result = 0;
        if (capacity > query_chunk) capacity = query_chunk;
        while (capacity-- && source[*offset]) destination[result++] = source[(*offset)++];
    } else if (number == SYS_close) {
        int fd = va_arg(arguments,int);
        assert(fd == MEMINFO_FD || fd == SMAPS_FD);
        ++close_queries; result = 0;
    } else if (number == SYS_fstatfs) {
        int fd = va_arg(arguments,int);
        struct statfs *status = va_arg(arguments,struct statfs *);
        if (actual_statfs) result = syscall(number,fd,status);
        else if (statfs_error) errno = statfs_error;
        else { status->f_type = statfs_type; status->f_bsize = statfs_page; result = 0; }
        if (raise_after_query) { raise_after_query = 0; assert(!raise(SIGUSR1)); }
#if defined(__x86_64__)
    } else if (number == SYS_arch_prctl) {
        assert(va_arg(arguments,int) == ARCH_GET_UNTAG_MASK);
        uintptr_t *mask = va_arg(arguments,uintptr_t *);
        ++address_queries;
        if (query_error) errno = query_error;
        else { *mask = untag_mask; result = 0; }
#elif defined(__riscv) && __riscv_xlen == 64
    } else if (number == SYS_prctl) {
        assert(va_arg(arguments,int) == PR_GET_TAGGED_ADDR_CTRL);
        for (unsigned i=0; i<4; ++i) assert(va_arg(arguments,unsigned long) == 0);
        ++address_queries;
        if (query_error) errno = query_error;
        else result = tagged_mode;
#endif
    } else assert(!"unexpected production syscall");
    va_end(arguments);
    return result;
}

void *__real_mmap(void *,size_t,int,int,int,off_t);
static void mapping_entry(void) {
    ++*mapping_calls;
    entry_errno = errno;
    if (use_real_mmap || replacement_fd >= 0) assert(!delivered);
}
int __wrap_mprotect(void *p,size_t n,int prot) {
    (void)p; (void)n; (void)prot; mapping_entry(); errno=result_errno; return -1;
}
int __wrap_pkey_mprotect(void *p,size_t n,int prot,int key) {
    (void)p; (void)n; (void)prot; (void)key; mapping_entry(); errno=result_errno; return -1;
}
int __wrap_munmap(void *p,size_t n) {
    (void)p; (void)n; mapping_entry(); errno=result_errno; return -1;
}
void *__wrap_mmap(void *p,size_t n,int prot,int flags,int fd,off_t offset) {
    mapping_entry();
    if (use_real_mmap) return __real_mmap(p,n,prot,flags,fd,offset);
    errno=result_errno; return mapping_result;
}
void *__wrap_mmap64(void *p,size_t n,int prot,int flags,int fd,off64_t offset) {
    return __wrap_mmap(p,n,prot,flags,fd,offset);
}
void *__wrap_mremap(void *old,size_t old_size,size_t new_size,int flags,...) {
    (void)old; (void)old_size; (void)new_size;
    if (flags & MREMAP_FIXED) {
        va_list arguments; va_start(arguments,flags);
        (void)va_arg(arguments,void *); va_end(arguments);
    }
    mapping_entry(); errno=result_errno; return mapping_result;
}

static void reset(void) {
    meminfo_text="MemTotal: 65536 kB\nHugepagesize: 2048 kB\n";
    smaps_text=normal_smaps;
    query_chunk=3; query_error=statfs_error=actual_statfs=raise_after_query=0;
    statfs_type=0x01021994; statfs_page=4096; untag_mask=UINTPTR_MAX; tagged_mode=0; cpuid_lam=0;
    simulated_page=4096;
    address_queries=mask_queries=open_queries=close_queries=0;
    mapping_result=(void *)0x70000000; result_errno=ENOSPC; entry_errno=0;
    use_real_mmap=0; replacement_fd=replaced_fd=-1; delivered=0; *mapping_calls=0;
    teapot_shadow_registry_count=0;
    teapot_shadow_register_owned(0x50001000,0x50002000,1);
}

#define CHECK(expression) do { assert(expression); ++checks; } while (0)
static void address_tests(void) {
    uintptr_t output;
    CHECK(mapping_address(0x12345000,MAPPING_FIXED_DESTINATION,&output) && output==0x12345000);
    CHECK(!address_queries);
#if defined(__x86_64__)
    untag_mask=UINT64_C(0x81ffffffffffffff);
    CHECK(mapping_address(UINT64_C(0x3e00000012345000),MAPPING_UNTAGGED,&output) && output==0x12345000);
    CHECK(mapping_address(UINT64_C(0xbe00000012345000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0x8000000012345000));
    CHECK(mapping_address(UINT64_C(0x3e00000012345000),MAPPING_FIXED_DESTINATION,&output) && output==UINT64_C(0x3e00000012345000));
    untag_mask=UINT64_C(0x8000ffffffffffff);
    CHECK(mapping_address(UINT64_C(0x7fff000012345000),MAPPING_UNTAGGED,&output) && output==0x12345000);
    untag_mask=0xff;
    CHECK(!mapping_address(0x12345000,MAPPING_UNTAGGED,&output));
#elif defined(__aarch64__)
    CHECK(mapping_address(UINT64_C(0x5a00000012345000),MAPPING_UNTAGGED,&output) && output==0x12345000);
    CHECK(mapping_address(UINT64_C(0x5a80000012345000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0x5a80000012345000));
    CHECK(!mapping_address(UINT64_C(0x5a00000012345000),MAPPING_FIXED_DESTINATION,&output));
    CHECK(mapping_address(UINT64_C(0xffff800012345000),MAPPING_FIXED_DESTINATION,&output) && output==UINT64_C(0xffff800012345000));
#elif defined(__riscv)
    tagged_mode=(7UL<<PR_PMLEN_SHIFT)|PR_TAGGED_ADDR_ENABLE;
    CHECK(mapping_address(UINT64_C(0xfe00000012345000),MAPPING_UNTAGGED,&output) && output==0x12345000);
    CHECK(mapping_address(UINT64_C(0x0100000012345000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0xff00000012345000));
    CHECK(mapping_address(UINT64_C(0xfe00000012345000),MAPPING_FIXED_DESTINATION,&output) && output==UINT64_C(0xfe00000012345000));
    tagged_mode=(16UL<<PR_PMLEN_SHIFT)|PR_TAGGED_ADDR_ENABLE;
    CHECK(mapping_address(UINT64_C(0x5aff000012345000),MAPPING_UNTAGGED,&output) && output==0x12345000);
    CHECK(mapping_address(UINT64_C(0x5aff800012345000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0xffff800012345000));
    tagged_mode=16UL<<PR_PMLEN_SHIFT;
    CHECK(mapping_address(UINT64_C(0x5aff000012345000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0x5aff000012345000));
    tagged_mode=(8UL<<PR_PMLEN_SHIFT)|PR_TAGGED_ADDR_ENABLE;
    CHECK(!mapping_address(0x12345000,MAPPING_UNTAGGED,&output));
    tagged_mode=2;
    CHECK(!mapping_address(0x12345000,MAPPING_UNTAGGED,&output));
    for (unsigned pmlen=7; pmlen<=16; pmlen+=9) {
        uintptr_t positive=UINTPTR_MAX>>(pmlen+1);
        uintptr_t negative=~positive;
        tagged_mode=((unsigned long)pmlen<<PR_PMLEN_SHIFT)|PR_TAGGED_ADDR_ENABLE;
        CHECK(mapping_address(positive,MAPPING_UNTAGGED,&output) && output==positive);
        CHECK(mapping_address(negative,MAPPING_UNTAGGED,&output) && output==negative);
        CHECK(mapping_address(positive+1,MAPPING_UNTAGGED,&output) && output==negative);
        CHECK(mapping_address(negative-1,MAPPING_UNTAGGED,&output) && output==positive);
    }
    tagged_mode=PR_TAGGED_ADDR_ENABLE;
    CHECK(!mapping_address(0,MAPPING_UNTAGGED,&output));
#endif
#if !defined(__aarch64__)
    query_error=EINVAL;
    CHECK(mapping_address(0x12345000,MAPPING_UNTAGGED,&output) && output==0x12345000);
#if defined(__x86_64__)
    cpuid_lam=1;
    CHECK(!mapping_address(0x12345000,MAPPING_UNTAGGED,&output));
#endif
    query_error=EPERM;
#if defined(__riscv)
    CHECK(mapping_address(UINT64_C(0x00007fffffffffff),MAPPING_UNTAGGED,&output) && output==UINT64_C(0x00007fffffffffff));
    CHECK(mapping_address(UINT64_C(0xffff800000000000),MAPPING_UNTAGGED,&output) && output==UINT64_C(0xffff800000000000));
    CHECK(!mapping_address(UINT64_C(0x0000800000000000),MAPPING_UNTAGGED,&output));
    CHECK(!mapping_address(UINT64_C(0xffff7fffffffffff),MAPPING_UNTAGGED,&output));
    CHECK(!mapping_address(UINT64_C(0x5aff000012345000),MAPPING_UNTAGGED,&output));
#else
    CHECK(!mapping_address(0x12345000,MAPPING_UNTAGGED,&output));
#endif
#endif
}

static void metadata_tests(void) {
    uintptr_t granule;
    reset();
    CHECK(mapping_mmap_granule(MAP_ANONYMOUS|MAP_HUGETLB|(21<<MAP_HUGE_SHIFT),-1,&granule) && granule==2097152);
    CHECK(!open_queries && !mmap_needs_signal_guard(MAP_FIXED|MAP_ANONYMOUS|MAP_HUGETLB|(21<<MAP_HUGE_SHIFT)));
    CHECK(mapping_mmap_granule(MAP_ANONYMOUS|MAP_HUGETLB,-1,&granule) && granule==2097152);
    CHECK(open_queries==1 && close_queries==1 && mmap_needs_signal_guard(MAP_FIXED|MAP_ANONYMOUS|MAP_HUGETLB));
    CHECK(mapping_mmap_granule(MAP_PRIVATE,7,&granule) && granule==(uintptr_t)simulated_page);
    statfs_type=HUGETLBFS_MAGIC; statfs_page=1073741824;
    CHECK(mapping_mmap_granule(MAP_PRIVATE,7,&granule) && granule==1073741824);
    statfs_page=1234;
    CHECK(!mapping_mmap_granule(MAP_PRIVATE,7,&granule));
    statfs_error=EACCES;
    CHECK(!mapping_mmap_granule(MAP_PRIVATE,7,&granule));
    statfs_error=EBADF;
    CHECK(mapping_mmap_granule(MAP_PRIVATE,-1,&granule) && granule==(uintptr_t)simulated_page);
    meminfo_text="Hugepagesize: 18446744073709551615 kB\n";
    CHECK(!mapping_default_huge_page(&granule));
    meminfo_text="Hugepagesize: 2048 kBytes\n";
    CHECK(!mapping_default_huge_page(&granule));
    meminfo_text="Hugepagesize: 0 kB\n";
    CHECK(!mapping_default_huge_page(&granule));
    CHECK(mapping_mremap_granule(0x400000,&granule) && granule==(uintptr_t)simulated_page);
    smaps_text=huge_smaps;
    CHECK(mapping_mremap_granule(0x400000,&granule) && granule==2097152);
    CHECK(mapping_mremap_granule(0x600000,&granule) && granule==(uintptr_t)simulated_page);
    CHECK(mapping_mremap_granule(0x300000,&granule) && granule==(uintptr_t)simulated_page);
    smaps_text="400000-600000 rw-p 0 00:00 0\nKernelPageSize: 2048 kB\n";
    CHECK(!mapping_mremap_granule(0x400000,&granule));
    smaps_text="400000-600000 rw-p 0 00:00 0\nVmFlags: ht\n";
    CHECK(!mapping_mremap_granule(0x400000,&granule));
    smaps_text="";
    CHECK(!mapping_mremap_granule(0x400000,&granule));
    smaps_text="not a maps record\n";
    CHECK(!mapping_mremap_granule(0x400000,&granule));
    CHECK(!touches_owned(0x50000000,4096,4096));
    CHECK(touches_owned(0x50000000,4097,4096));
    CHECK(touches_owned(0x50000000,1,2097152));
    CHECK(!touches_owned(0x50002000,4096,4096));
    CHECK(touches_owned(0x50000000,SIZE_MAX,4096));
    CHECK(!touches_owned(UINTPTR_MAX-4095,SIZE_MAX,4096));
    CHECK(touches_owned((uintptr_t)&teapot_shadow_registry_count,1,4096));
    simulated_page=65536;
    teapot_shadow_registry_count=0;
    teapot_shadow_register_owned(0x50010000,0x50020000,1);
    CHECK(!touches_owned(0x50000000,65536,65536));
    CHECK(touches_owned(0x50000000,65537,65536));
    CHECK(touches_owned(0x50000000,1,2097152));
    CHECK(!touches_owned(0x50020000,65536,65536));
    statfs_error=0; statfs_type=0x01021994;
    CHECK(mapping_mmap_granule(MAP_PRIVATE,7,&granule) && granule==65536);
    smaps_text=normal_smaps;
    CHECK(mapping_mremap_granule(0x400000,&granule) && granule==65536);
}

static void rejected(unsigned operation,const char *message) {
    int descriptors[2]; assert(!pipe(descriptors));
    *mapping_calls=0;
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        close(descriptors[0]); assert(dup2(descriptors[1],2)==2); close(descriptors[1]);
        struct rlimit core={0,0}; assert(!setrlimit(RLIMIT_CORE,&core));
        if (operation==0)
            (void)mmap__teapot_wrapper__((void *)0x50000000,4096,PROT_READ,
                MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|(21<<MAP_HUGE_SHIFT),-1,0);
        else if (operation==1)
            (void)mmap64__teapot_wrapper__((void *)0x50000000,4096,PROT_READ,MAP_FIXED|MAP_PRIVATE,7,0);
        else if (operation==2)
            (void)mremap__teapot_wrapper__((void *)0x400000,4096,4096,MREMAP_FIXED|MREMAP_MAYMOVE,(void *)0x50000000);
        else if (operation==3)
            (void)mmap__teapot_wrapper__((void *)0x50000000,4096,PROT_READ,
                MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,-1,0);
        else if (operation==4)
            (void)mremap__teapot_wrapper__((void *)0x10001000,0x400000,0x400000,
                MREMAP_FIXED|MREMAP_MAYMOVE,(void *)0x80001000);
        else assert(0);
        _exit(99);
    }
    close(descriptors[1]); char output[256]={0};
    assert(read(descriptors[0],output,sizeof(output)-1)>0); close(descriptors[0]);
    int status; assert(waitpid(child,&status,0)==child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    CHECK(strstr(output,message));
    CHECK(!*mapping_calls);
}

static void refusal_tests(void) {
    reset(); rejected(0,"runtime-owned shadow/protected memory");
    statfs_type=HUGETLBFS_MAGIC; statfs_page=2097152;
    rejected(1,"runtime-owned shadow/protected memory");
    smaps_text=huge_smaps; rejected(2,"runtime-owned shadow/protected memory");
    rejected(3,"runtime-owned shadow/protected memory");
    statfs_error=EACCES; rejected(1,"unknown mapping footprint/address transform");
    smaps_text=NULL; rejected(2,"unknown mapping footprint/address transform");
    meminfo_text="Hugepagesize: nonsense\n"; rejected(3,"unknown mapping footprint/address transform");
    reset(); teapot_shadow_registry_count=0;
    teapot_shadow_register_owned(0x80402000,0x80403000,1);
    smaps_text="10000000-10002000 rw-p 0 00:00 0\nKernelPageSize: 4 kB\nVmFlags: rd wr\n"
               "10400000-10600000 rw-p 0 00:00 0\nKernelPageSize: 2048 kB\nVmFlags: rd ht wr\n";
    rejected(4,"runtime-owned shadow/protected memory");
}

static void signal_tests(void) {
    reset();
    struct sigaction action={0}, previous_action;
    action.sa_handler=deliver; sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1,&action,&previous_action));
    sigset_t blocked, previous, after;
    sigemptyset(&blocked); sigaddset(&blocked,SIGUSR2);
    assert(!sigprocmask(SIG_BLOCK,&blocked,&previous));
    replacement_fd=(int)syscall(SYS_memfd_create,"replacement",0);
    replaced_fd=(int)syscall(SYS_memfd_create,"original",0);
    assert(replacement_fd>=0 && replaced_fd>=0);
    assert(!ftruncate(replacement_fd,4096) && !ftruncate(replaced_fd,4096));
    assert(pwrite(replacement_fd,"B",1,0)==1 && pwrite(replaced_fd,"A",1,0)==1);
    void *destination=__real_mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(destination!=MAP_FAILED);
    use_real_mmap=actual_statfs=raise_after_query=1;
    errno=E2BIG;
    void *result=mmap__teapot_wrapper__(destination,4096,PROT_READ,MAP_FIXED|MAP_PRIVATE,replaced_fd,0);
    CHECK(result==destination && *(char *)result=='A');
    CHECK(errno==E2BIG && entry_errno==E2BIG && delivered==1 && mask_queries==2);
    char byte=0; CHECK(pread(replaced_fd,&byte,1,0)==1 && byte=='B');
    assert(!sigprocmask(SIG_SETMASK,NULL,&after));
    CHECK(sigismember(&after,SIGUSR2) && !sigismember(&after,SIGUSR1));
    assert(!syscall(SYS_munmap,destination,4096));
    close(replacement_fd); close(replaced_fd);

    reset();
    replacement_fd=0; /* Require mapping entry to precede deferred delivery. */
    raise_after_query=1; mapping_result=MAP_FAILED; result_errno=ENOMEM;
    errno=E2BIG;
    result=mremap__teapot_wrapper__((void *)0x400000,4096,8192,MREMAP_MAYMOVE);
    CHECK(result==MAP_FAILED && errno==ENOMEM && entry_errno==E2BIG && delivered==1 && mask_queries==2);
    replacement_fd=-1;

    reset();
    errno=E2BIG;
    (void)mmap__teapot_wrapper__((void *)0x70000000,4096,PROT_READ,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    (void)mmap__teapot_wrapper__(NULL,4096,PROT_READ,MAP_PRIVATE,7,0);
    (void)mprotect__teapot_wrapper__((void *)0x70000000,4096,PROT_READ);
    (void)munmap__teapot_wrapper__((void *)0x70000000,4096);
    CHECK(!mask_queries && !open_queries);
    assert(!sigprocmask(SIG_SETMASK,&previous,NULL));
    assert(!sigaction(SIGUSR1,&previous_action,NULL));
}

int main(void) {
    mapping_calls=__real_mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(mapping_calls!=MAP_FAILED);
    reset(); address_tests(); metadata_tests(); refusal_tests(); signal_tests();
    printf("mapping footprint PASS checks=%u huge/query-failure/ISA-address/signal/fd-race\n",checks);
    return 0;
}
