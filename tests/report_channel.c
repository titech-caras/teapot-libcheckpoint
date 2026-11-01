#define _GNU_SOURCE
#undef NDEBUG
#include "checkpoint.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

statistics_t simulation_statistics;
checkpoint_metadata_t checkpoint_metadata[MAX_CHECKPOINTS];
uint64_t checkpoint_cnt, instruction_cnt, libcheckpoint_enabled;
extern void libcheckpoint_report_channel_init(void);
extern void report_gadget(const char *, int, uint64_t, uint64_t, dift_tag_t);

int main(void) {
    int channel[2], output[2];
    assert(pipe(channel) == 0 && pipe(output) == 0);
    char fd[24];
    snprintf(fd, sizeof(fd), "%d", channel[1]);
    assert(setenv("HF_TEAPOT_REPORT_FD", fd, 1) == 0);
    libcheckpoint_report_channel_init();
    assert(dup2(output[1], STDERR_FILENO) == STDERR_FILENO);
    unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    const int kinds[] = {GADGET_KASPER_MDS, GADGET_KASPER_CACHE, GADGET_KASPER_PORT,
                         GADGET_KASPER_CACHE_STORE, GADGET_TARGET};
    const char *names[] = {"KASPER_MDS", "KASPER_CACHE", "KASPER_PORT",
                           "KASPER_CACHE_STORE", "GADGET_TARGET"};
    libcheckpoint_enabled = 1;
    instruction_cnt = 37;
    checkpoint_cnt = 1;
    checkpoint_metadata[0].return_address = 0x1234;
    for (unsigned i = 0; i < sizeof(kinds)/sizeof(*kinds); ++i) {
        /* A disposable report-call site: never executed, but suppression uses
         * the real ISA patcher. Each kind keeps the tag in both records. */
        uint32_t call = 0x000000ef;
        memcpy(code + i * 16, &call, sizeof(call));
        uintptr_t site = (uintptr_t)(code + i * 16);
        report_gadget(names[i], kinds[i], site, 0x5678, 1u << i);
        char actual[512], expected[512];
        ssize_t n = read(channel[0], actual, sizeof(actual)-1);
        assert(n > 0); actual[n] = 0;
        snprintf(expected, sizeof(expected), "TPGADGET 2 %d 0x%" PRIxPTR " %u\n",
                 kinds[i], site, 1u << i);
        assert(strcmp(actual, expected) == 0);
        n = read(output[0], actual, sizeof(actual)-1);
        assert(n > 0); actual[n] = 0;
        snprintf(expected, sizeof(expected),
                 "[teapot], %d %s, 0x%" PRIxPTR ", 0x5678, 0x%x, 37, 0x1234, \n",
                 kinds[i], names[i], site, 1u << i);
        assert(strcmp(actual, expected) == 0);
        assert(instruction_cnt == 37 && checkpoint_cnt == 1);
        assert(checkpoint_metadata[0].return_address == 0x1234);
        assert(simulation_statistics.bug_type[kinds[i]] == 1);
    }
    assert(simulation_statistics.total_bug == 5);
    return 0;
}
