#pragma once

#include <signal.h>

/* A PURE kernel SIGILL is an unsupported instruction, never a rollback.
 * Distinguish this fatal exit from successful speculative recovery. */
#define LIBCHECKPOINT_RISCV_PURE_SIGILL_EXIT_STATUS (128 + SIGILL)
#define LIBCHECKPOINT_RISCV_PURE_SIGILL_DIAGNOSTIC \
    "Teapot: fatal lp64-pure kernel SIGILL during speculation or memlog replay\n"

void setup_signal_handler(void);
int sigaction__teapot_wrapper__(int sig, const struct sigaction *action,
                                struct sigaction *old_action);
typedef void (*teapot_signal_function)(int);
teapot_signal_function signal__teapot_wrapper__(int sig, teapot_signal_function handler);
teapot_signal_function __sysv_signal__teapot_wrapper__(int sig, teapot_signal_function handler);
teapot_signal_function sysv_signal__teapot_wrapper__(int sig, teapot_signal_function handler);
teapot_signal_function bsd_signal__teapot_wrapper__(int sig, teapot_signal_function handler);
teapot_signal_function ssignal__teapot_wrapper__(int sig, teapot_signal_function handler);
teapot_signal_function sigset__teapot_wrapper__(int sig, teapot_signal_function handler);
