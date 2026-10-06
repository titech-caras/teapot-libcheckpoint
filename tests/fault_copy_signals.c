/* Routing matrix for the new copied-load hook only. Real copied-context
 * assertions and fault execution are covered by the archive-linked fixture. */
#define _GNU_SOURCE
#undef NDEBUG
#include "checkpoint.h"
#include "fault_sites.h"
#include "signal_handler.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

scratchpad_t scratchpad;
uint64_t checkpoint_cnt;
volatile uint64_t in_restore_memlog;
static unsigned copy_calls,forward_calls;
static bool seen_active,seen_replay;
static siginfo_t *expected_info;
static void *expected_context;

void restore_checkpoint_SIGSEGV(void) { assert(0); }
void restore_checkpoint_after_memlog(void) { assert(0); }
void restore_checkpoint_memlog(void) { assert(0); }
bool teapot_fault_risc_copied_kernel(uintptr_t pc,bool active,bool replay) {
    assert(pc==0x1000);++copy_calls;seen_active=active;seen_replay=replay;return true;
}
bool teapot_fault_train(int sig,const siginfo_t *info,uintptr_t pc,bool active,bool replay) {
    (void)sig;(void)info;(void)pc;(void)active;(void)replay;return false;
}
void signal_handler(int,siginfo_t *,void *);

static uintptr_t *context_pc(ucontext_t *context) {
#if defined(__aarch64__)
    return (uintptr_t *)&context->uc_mcontext.pc;
#else
    return (uintptr_t *)&context->uc_mcontext.__gregs[0];
#endif
}
static void forwarded(int sig,siginfo_t *info,void *context) {
    assert(info==expected_info && context==expected_context && info->si_signo==sig);
    assert(*context_pc(context)==0x1000);++forward_calls;
}
int main(void) {
    const int signals[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE};
    struct sigaction action={.sa_sigaction=forwarded,.sa_flags=SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    for(unsigned i=0;i<4;i++) assert(!sigaction(signals[i],&action,NULL));
    setup_signal_handler();
    const struct {int signal,code;bool copied;} cases[]={
        {SIGSEGV,SEGV_MAPERR,true},{SIGSEGV,SEGV_ACCERR,true},
        {SIGBUS,BUS_ADRALN,true},{SIGBUS,BUS_ADRERR,true},
#ifdef SEGV_MTESERR
        {SIGSEGV,SEGV_MTESERR,true},
#endif
#ifdef SEGV_MTEAERR
        {SIGSEGV,SEGV_MTEAERR,false},
#endif
#ifdef BUS_MCEERR_AO
        {SIGBUS,BUS_MCEERR_AO,false},
#endif
        {SIGILL,ILL_ILLOPC,false},{SIGFPE,FPE_INTDIV,false},
        {SIGSEGV,SI_USER,false},{SIGSEGV,SI_QUEUE,false},
        {SIGBUS,SI_TKILL,false},{SIGILL,SI_USER,false},{SIGFPE,SI_QUEUE,false},
    };
    unsigned checks=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
        for(unsigned state=0;state<3;state++) {
            ucontext_t context={0};sigemptyset(&context.uc_sigmask);
            siginfo_t info={.si_signo=cases[i].signal,.si_code=cases[i].code};
            *context_pc(&context)=0x1000;checkpoint_cnt=state!=0;in_restore_memlog=state==2;
            expected_info=&info;expected_context=&context;copy_calls=forward_calls=0;
            signal_handler(cases[i].signal,&info,&context);
            assert(copy_calls==(unsigned)cases[i].copied);
            if(cases[i].copied) {
                assert(seen_active==(state!=0) && seen_replay==(state==2));
                assert(*context_pc(&context)==(uintptr_t)restore_checkpoint_SIGSEGV);
            } else if(info.si_code>0 && state) {
                assert(!forward_calls);
                assert(*context_pc(&context)==(state==2 ? (uintptr_t)restore_checkpoint_memlog :
                                                        (uintptr_t)restore_checkpoint_SIGSEGV));
            } else assert(forward_calls==1 && *context_pc(&context)==0x1000);
            ++checks;
        }
    printf("copied-load routing matrix: %u checks PASS; async/other signals retain baseline\n",checks);
}
