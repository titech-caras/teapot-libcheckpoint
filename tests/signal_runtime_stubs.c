/* Isolated signal-disposition fixtures do not link checkpoint assembly. */
#include "checkpoint.h"
checkpoint_metadata_t checkpoint_metadata[MAX_CHECKPOINTS];
uintptr_t checkpoint_target_metadata[CHECKPOINT_TARGET_METADATA_SIZE / 8];
void restore_checkpoint_registers(void) { __builtin_trap(); }
void teapot_signal_resume(void) { __builtin_trap(); }
__asm__(".pushsection teapot_checkpoint,\"ax\"\n.popsection\n"
        ".globl teapot_restore_registers_end, teapot_checkpoint_skip_jump\n"
        ".globl teapot_signal_resume_mask_load, teapot_signal_resume_mask_ready\n"
        ".globl teapot_signal_resume_unblocked\n"
        ".set teapot_restore_registers_end, restore_checkpoint_registers\n"
        ".set teapot_checkpoint_skip_jump, teapot_signal_resume\n"
        ".set teapot_signal_resume_mask_load, teapot_signal_resume\n"
        ".set teapot_signal_resume_mask_ready, teapot_signal_resume\n"
        ".set teapot_signal_resume_unblocked, teapot_signal_resume\n");

#ifdef SIGNAL_TEST_QUEUE_STUB
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>
long __wrap_syscall(long number, ...) {
    return number == SYS_gettid ? getpid() : 0;
}
#endif
