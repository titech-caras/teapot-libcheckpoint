#pragma once

#include <signal.h>

void setup_signal_handler(void);
int sigaction__teapot_wrapper__(int sig, const struct sigaction *action,
                                struct sigaction *old_action);
typedef void (*teapot_signal_function)(int);
teapot_signal_function signal__teapot_wrapper__(int sig, teapot_signal_function handler);
