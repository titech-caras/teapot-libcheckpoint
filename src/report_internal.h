#pragma once

/* Runtime startup only; the reporting path must not consult the environment. */
void libcheckpoint_report_channel_init(void)
    __attribute__((visibility("hidden")));
