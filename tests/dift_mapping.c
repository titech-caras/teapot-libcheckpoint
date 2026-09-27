#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exercise the compatibility definition used with old libc headers. */
#undef MAP_FIXED_NOREPLACE
#include "../src/dift_support.c"

int main(void) {
    const size_t size = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *page = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    page[0] = 0x5a;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        map_runtime_shadow_range((uintptr_t)page, (uintptr_t)page + size,
                                 PROT_READ | PROT_WRITE);
        /* A fixed mapping must fail rather than destroy the existing page. */
        _exit(1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(page[0] == 0x5a);
    assert(munmap(page, size) == 0);

    map_runtime_shadow_range((uintptr_t)page, (uintptr_t)page + size,
                             PROT_READ | PROT_WRITE);
    page[0] = 0xa5;
    /* Repeat requests for a known runtime range do not remap it. */
    map_runtime_shadow_range((uintptr_t)page, (uintptr_t)page + size,
                             PROT_READ | PROT_WRITE);
    assert(page[0] == 0xa5);
    assert(munmap(page, size) == 0);
    return 0;
}
