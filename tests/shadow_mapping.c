#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#undef NDEBUG
#include "shadow_mapping.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

extern void *mmap__teapot_wrapper__(void *,size_t,int,int,int,off_t);
extern void *mmap64__teapot_wrapper__(void *,size_t,int,int,int,off64_t);
static size_t page;
static unsigned char *owned,*other;
static unsigned checks;
static int *side_effect_calls;

int __real_mprotect(void *,size_t,int);
int __wrap_mprotect(void *p,size_t n,int prot) { ++*side_effect_calls; return __real_mprotect(p,n,prot); }
int __real_pkey_mprotect(void *,size_t,int,int);
int __wrap_pkey_mprotect(void *p,size_t n,int prot,int key) {
    ++*side_effect_calls; return __real_pkey_mprotect(p,n,prot,key);
}
int __real_munmap(void *,size_t);
int __wrap_munmap(void *p,size_t n) { ++*side_effect_calls; return __real_munmap(p,n); }
void *__real_mmap(void *,size_t,int,int,int,off_t);
void *__wrap_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
    if(side_effect_calls) ++*side_effect_calls;
    return __real_mmap(p,n,prot,flags,fd,off);
}
void *__real_mmap64(void *,size_t,int,int,int,off64_t);
void *__wrap_mmap64(void *p,size_t n,int prot,int flags,int fd,off64_t off) {
    ++*side_effect_calls; return __real_mmap64(p,n,prot,flags,fd,off);
}
void *__real_mremap(void *,size_t,size_t,int,...);
void *__wrap_mremap(void *p,size_t old_n,size_t new_n,int flags,...) {
    ++*side_effect_calls;
    if(flags&MREMAP_FIXED) {
        va_list args; va_start(args,flags); void *dest=va_arg(args,void *); va_end(args);
        return __real_mremap(p,old_n,new_n,flags,dest);
    }
    return __real_mremap(p,old_n,new_n,flags);
}
static void action(unsigned op,unsigned char *p,size_t n) {
    switch(op) {
    case 0: (void)mprotect__teapot_wrapper__(p,n,PROT_READ); break;
    case 1: (void)munmap__teapot_wrapper__(p,n); break;
    case 2: (void)mremap__teapot_wrapper__(p,n,page,MREMAP_MAYMOVE); break;
    case 3: (void)mremap__teapot_wrapper__(other,page,n,MREMAP_MAYMOVE|MREMAP_FIXED,p); break;
    case 4: (void)mmap__teapot_wrapper__(p,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0); break;
    case 5: (void)mmap64__teapot_wrapper__(p,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0); break;
    case 6: (void)mremap__teapot_wrapper__(p,0,page,MREMAP_MAYMOVE); break;
    case 7: (void)pkey_mprotect__teapot_wrapper__(p,n,PROT_READ,-1); break;
    case 8: (void)mremap__teapot_wrapper__(p,n,page,MREMAP_MAYMOVE|MREMAP_DONTUNMAP); break;
    case 9: (void)mremap__teapot_wrapper__(other,page,n,MREMAP_MAYMOVE|MREMAP_FIXED|MREMAP_DONTUNMAP,p); break;
    case 10: (void)mremap__teapot_wrapper__(other,page,n,MREMAP_FIXED,p); break;
    case 11: (void)mremap__teapot_wrapper__(other,page,n,MREMAP_FIXED|MREMAP_MAYMOVE|0x40000000,p); break;
    default: assert(0);
    }
}
static void rejected_message(unsigned op,unsigned char *p,size_t n,const char *expected) {
    int pipefd[2]; assert(!pipe(pipefd));
    *side_effect_calls=0;
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        close(pipefd[0]); assert(dup2(pipefd[1],2)==2); close(pipefd[1]);
        struct rlimit limit={0,0}; assert(!setrlimit(RLIMIT_CORE,&limit));
        action(op,p,n); _exit(99);
    }
    close(pipefd[1]); char message[256]={0}; ssize_t nread=read(pipefd[0],message,sizeof(message)-1);
    assert(nread>0); close(pipefd[0]);
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
    assert(strstr(message,expected));
    assert(*side_effect_calls==0); ++checks;
}
static void rejected(unsigned op,unsigned char *p,size_t n) {
    rejected_message(op,p,n,"runtime-owned shadow/protected memory");
}
int main(void) {
    page=(size_t)getpagesize();
    side_effect_calls=__real_mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(side_effect_calls!=MAP_FAILED);
    unsigned char *span=mmap(NULL,5*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(span!=MAP_FAILED); owned=span+2*page; other=span;
    memset(span,0x5a,5*page);
    teapot_shadow_register_owned((uintptr_t)owned,(uintptr_t)(owned+page),1);
    assert(teapot_shadow_registry_count==1 && teapot_shadow_registry[0].normal_rw_dift==1);
    teapot_shadow_register_owned((uintptr_t)(owned+2*page),(uintptr_t)(owned+3*page),0);
    assert(!teapot_shadow_registry[1].normal_rw_dift);
    for(unsigned op=0;op<12;++op) {
        rejected(op,owned,page);
        rejected(op,owned+1,1); /* partial, page-rounded, possibly kernel-invalid */
        if(op!=6) rejected(op,owned-page,page+1);
        rejected(op,(void *)&teapot_shadow_registry_count,1); /* protected is owned, not DIFT */
    }
    for(unsigned op=0;op<12;++op) if(op!=6) rejected(op,owned,SIZE_MAX);
#if defined(__aarch64__)
    for(unsigned op=0;op<12;++op) {
        void *tagged=(void *)((uintptr_t)owned|UINT64_C(0x5a00000000000000));
        if(op==3 || op==4 || op==5 || op==9 || op==10 || op==11)
            rejected_message(op,tagged,page,"unknown mapping footprint/address transform");
        else rejected(op,tagged,page);
    }
#endif

    /* Non-overlapping and adjacent calls preserve libc return/errno. Internal
       original libc mprotect deliberately remains usable on an owned page. */
    errno=E2BIG; assert(!mprotect__teapot_wrapper__(span,page,PROT_READ)); assert(errno==E2BIG);
    assert(!mprotect(span,page,PROT_READ|PROT_WRITE));
    assert(!mprotect(owned,page,PROT_READ)); assert(!mprotect(owned,page,PROT_READ|PROT_WRITE));
    errno=E2BIG; assert(!mprotect__teapot_wrapper__(owned+page,page,PROT_READ)); assert(errno==E2BIG);
    assert(!mprotect(owned+page,page,PROT_READ|PROT_WRITE));
    errno=0; int original=mprotect(span+1,1,PROT_READ), original_errno=errno;
    errno=0; int wrapped=mprotect__teapot_wrapper__(span+1,1,PROT_READ);
    assert(wrapped==original && errno==original_errno);
    errno=E2BIG; original=pkey_mprotect(span,page,PROT_READ,-1); original_errno=errno;
    assert(!mprotect(span,page,PROT_READ|PROT_WRITE));
    errno=E2BIG; wrapped=pkey_mprotect__teapot_wrapper__(span,page,PROT_READ,-1);
    assert(wrapped==original && errno==original_errno);
    assert(!mprotect(span,page,PROT_READ|PROT_WRITE));
    errno=0; original=pkey_mprotect(span,page,PROT_READ,INT32_MAX); original_errno=errno;
    errno=0; wrapped=pkey_mprotect__teapot_wrapper__(span,page,PROT_READ,INT32_MAX);
    assert(wrapped==original && errno==original_errno);
    errno=0; original=munmap(span,0); original_errno=errno;
    errno=0; wrapped=munmap__teapot_wrapper__(span,0);
    assert(wrapped==original && errno==original_errno);
    errno=0; void *invalid_direct=mremap(span,page,page,MREMAP_FIXED,span+page);
    original_errno=errno;
    errno=0; void *invalid_wrapped=mremap__teapot_wrapper__(span,page,page,MREMAP_FIXED,span+page);
    assert(invalid_wrapped==invalid_direct && errno==original_errno);
    errno=0; invalid_direct=mremap(span,page,page,MREMAP_MAYMOVE|0x40000000);
    original_errno=errno;
    errno=0; invalid_wrapped=mremap__teapot_wrapper__(span,page,page,MREMAP_MAYMOVE|0x40000000);
    assert(invalid_wrapped==invalid_direct && errno==original_errno);
    errno=0; void *direct=mmap((void *)(UINTPTR_MAX-2*page),page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    original_errno=errno;
    errno=0; void *via=mmap__teapot_wrapper__((void *)(UINTPTR_MAX-2*page),page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    assert(via==direct && errno==original_errno);
    errno=E2BIG; via=mmap__teapot_wrapper__(owned,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(via!=MAP_FAILED && via!=owned && errno==E2BIG); assert(!munmap__teapot_wrapper__(via,page));
    via=mmap64__teapot_wrapper__(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(via!=MAP_FAILED); memset(via,0x41,page);
    errno=E2BIG; via=mremap__teapot_wrapper__(via,page,2*page,MREMAP_MAYMOVE);
    assert(via!=MAP_FAILED && errno==E2BIG && *(unsigned char *)via==0x41);
    void *destination=mmap(NULL,2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(destination!=MAP_FAILED);
    errno=E2BIG; via=mremap__teapot_wrapper__(via,2*page,2*page,MREMAP_MAYMOVE|MREMAP_FIXED,destination);
    assert(via==destination && errno==E2BIG && *(unsigned char *)via==0x41);
    assert(!munmap__teapot_wrapper__(via,2*page));
    assert(!munmap(span,5*page));
    printf("mapping enforcement PASS refusals=%u nonoverlap/errno/variadic/internal-bypass\n",checks);
    return 0;
}
