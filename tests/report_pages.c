#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "checkpoint.h"

extern void make_report_call_nop(uint64_t address);
extern void report_gadget(const char *description, int type, uint64_t address,
                           uint64_t access, dift_tag_t tag);
extern int __real_mprotect(void *address, size_t length, int protection);
extern long __real_sysconf(int name);
extern long __real_syscall(long number, ...);

#ifdef REPORT_TEST_ASAN
#include <sanitizer/asan_interface.h>
extern char __start_teapot_protected[], __stop_teapot_protected[];
#endif

statistics_t simulation_statistics;
checkpoint_metadata_t checkpoint_metadata[MAX_CHECKPOINTS];
uint64_t checkpoint_cnt, instruction_cnt;
uint64_t libcheckpoint_enabled;

static size_t page_size;
static unsigned call_count, fail_call;
static bool in_report, forbid_allocations;
static bool missing_maps, read_error, fake_maps, read_interrupted, write_interrupted;
static size_t read_limit, write_limit, maps_size, maps_position, output_size;
static char maps_text[16384], output[16384];
static struct {
    uintptr_t address;
    size_t length;
    int protection;
} calls[8];

#define CHECKED_ALLOCATION(type, name, parameters, arguments) \
    extern type __real_##name parameters; \
    type __wrap_##name parameters { \
        assert(!in_report || !forbid_allocations); \
        return __real_##name arguments; \
    }

CHECKED_ALLOCATION(FILE *, fopen, (const char *path, const char *mode), (path, mode))
CHECKED_ALLOCATION(void *, malloc, (size_t size), (size))
CHECKED_ALLOCATION(void *, calloc, (size_t count, size_t size), (count, size))
CHECKED_ALLOCATION(void *, realloc, (void *ptr, size_t size), (ptr, size))
CHECKED_ALLOCATION(ssize_t, getline, (char **line, size_t *size, FILE *file), (line, size, file))
CHECKED_ALLOCATION(int, fclose, (FILE *file), (file))
CHECKED_ALLOCATION(int, fputs, (const char *text, FILE *file), (text, file))
CHECKED_ALLOCATION(int, fputc, (int ch, FILE *file), (ch, file))
#undef CHECKED_ALLOCATION

int __wrap_fprintf(FILE *file, const char *format, ...) {
    assert(!in_report || !forbid_allocations);
    va_list args;
    va_start(args, format);
    int result = vfprintf(file, format, args);
    va_end(args);
    return result;
}

int __wrap_snprintf(char *buffer, size_t size, const char *format, ...) {
    assert(!in_report || !forbid_allocations);
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

/* Mock only the syscalls used by the reader/writer. Copy bytes directly so
 * testing a poisoned runtime buffer does not invoke libc's ASan interceptors. */
long __wrap_syscall(long number, ...) {
    va_list args;
    va_start(args, number);
    long result = -1;
    if (number == SYS_openat) {
        int dir = va_arg(args, int);
        const char *path = va_arg(args, const char *);
        int flags = va_arg(args, int);
        int mode = va_arg(args, int);
        assert(strcmp(path, "/proc/self/maps") == 0);
        if (missing_maps) {
            errno = ENOENT;
        } else if (fake_maps) {
            maps_position = 0;
            result = 713;
        } else {
            result = __real_syscall(number, dir, path, flags, mode);
        }
    } else if (number == SYS_read) {
        int fd = va_arg(args, int);
        char *buffer = va_arg(args, char *);
        size_t size = va_arg(args, size_t);
        if (read_error) {
            errno = EIO;
        } else if (read_interrupted) {
            read_interrupted = false;
            errno = EINTR;
        } else if (fake_maps) {
            assert(fd == 713);
            if (size > read_limit)
                size = read_limit;
            if (size > maps_size - maps_position)
                size = maps_size - maps_position;
            for (size_t i = 0; i < size; i++)
                ((volatile char *)buffer)[i] = maps_text[maps_position++];
            result = (long)size;
        } else {
            result = __real_syscall(number, fd, buffer, size);
        }
    } else if (number == SYS_write) {
        int fd = va_arg(args, int);
        const char *buffer = va_arg(args, const char *);
        size_t size = va_arg(args, size_t);
        assert(fd == STDERR_FILENO);
        if (write_interrupted) {
            write_interrupted = false;
            errno = EINTR;
        } else {
            if (write_limit && size > write_limit)
                size = write_limit;
            assert(size < sizeof(output) - output_size);
            for (size_t i = 0; i < size; i++)
                output[output_size++] = ((const volatile char *)buffer)[i];
            output[output_size] = '\0';
            result = (long)size;
        }
    } else if (number == SYS_close) {
        int fd = va_arg(args, int);
        result = fake_maps ? 0 : __real_syscall(number, fd);
    } else {
        assert(!"unexpected report syscall");
    }
    va_end(args);
    return result;
}

extern void __real_free(void *ptr);
void __wrap_free(void *ptr) {
    assert(!in_report || !forbid_allocations);
    __real_free(ptr);
}

long __wrap_sysconf(int name) {
    return name == _SC_PAGESIZE ? (long)page_size : __real_sysconf(name);
}

int __wrap_mprotect(void *address, size_t length, int protection) {
    assert(call_count < sizeof(calls) / sizeof(calls[0]));
    calls[call_count].address = (uintptr_t)address;
    calls[call_count].length = length;
    calls[call_count].protection = protection;
    call_count++;
    if (call_count == fail_call || (uintptr_t)address % page_size || length % page_size) {
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(address, length, protection);
}

static int protection_at(uintptr_t address) {
    FILE *maps = fopen("/proc/self/maps", "r");
    assert(maps != NULL);
    char *line = NULL;
    size_t capacity = 0;
    int result = -1;
    while (getline(&line, &capacity, maps) >= 0) {
        unsigned long start, end;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3 &&
                start <= address && address < end) {
            result = (perms[0] == 'r' ? PROT_READ : 0) |
                     (perms[1] == 'w' ? PROT_WRITE : 0) |
                     (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    free(line);
    assert(fclose(maps) == 0);
    return result;
}

static void check_patch(const char *mode, bool compressed, unsigned failing_call) {
#if defined(__x86_64__)
    const unsigned char original[] = {0xe8, 0, 0, 0, 0};
    const unsigned char nop[] = {0x0f, 0x1f, 0x44, 0, 0};
#elif defined(__aarch64__)
    const unsigned char original[] = {0, 0, 0, 0x94};
    const unsigned char nop[] = {0x1f, 0x20, 0x03, 0xd5};
#else
    const unsigned char original[] = {compressed ? 0x82 : 0xef, compressed ? 0x90 : 0, 0, 0};
    const unsigned char nop[] = {compressed ? 0x01 : 0x13, 0, 0, 0};
#endif
    size_t width = compressed ? 2 : sizeof(nop);
    bool crossing = strcmp(mode, "crossing") == 0 || strcmp(mode, "failure") == 0 ||
                    strcmp(mode, "missing-tail") == 0 || strcmp(mode, "restore-failure") == 0;
    bool missing = strcmp(mode, "isolated") == 0 || strcmp(mode, "missing-tail") == 0;
    bool full_report = strcmp(mode, "no-heap") == 0 || strcmp(mode, "format") == 0 ||
                       strcmp(mode, "long-format") == 0 || strcmp(mode, "disabled") == 0;
    bool should_patch = !failing_call && !(crossing && missing) && !missing_maps && !read_error &&
                        strcmp(mode, "disabled") != 0;
    void *mapping = mmap(NULL, 4 * page_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    uintptr_t base = ((uintptr_t)mapping + page_size - 1) & ~(page_size - 1);
    /* Crossing tests exercise byte-range handling, not instruction alignment. */
    uintptr_t address = base + (crossing ? page_size - 2 : page_size / 2 + 64);
    if (compressed && missing && !crossing)
        address = base + page_size - width;
    memcpy((void *)address, original, width);
    assert(__real_mprotect((void *)base, page_size, PROT_READ | PROT_EXEC) == 0);
    if (missing)
        assert(munmap((void *)(base + page_size), page_size) == 0);
    call_count = 0;
    fail_call = failing_call;
    output_size = 0;
    output[0] = '\0';
    if (fake_maps) {
        maps_size = (size_t)snprintf(maps_text, sizeof(maps_text),
            "fffffffffffffffff-fffffffffffffffffff rwxp 0 00:00 0\n"
            "%" PRIxPTR "-%" PRIxPTR " r-qp 0 00:00 0\n"
            "0-1 r--p 0 00:00 0 /", base, base + page_size);
        memset(maps_text + maps_size, 'x', 5000);
        maps_size += 5000;
        maps_size += (size_t)snprintf(maps_text + maps_size, sizeof(maps_text) - maps_size,
            "\n%" PRIxPTR "-%" PRIxPTR " r-xp 0 00:00 0 /target", base, base + page_size);
        /* The matching record has no final newline. */
    }

    char description[6000], expected[8192];
    if (strcmp(mode, "long-format") == 0) {
        memset(description, 'x', sizeof(description) - 1);
        description[sizeof(description) - 1] = '\0';
    } else {
        strcpy(description, "KASPER_CACHE");
    }
    if (full_report) {
        instruction_cnt = UINT64_MAX;
        checkpoint_cnt = MAX_CHECKPOINTS;
        size_t size = (size_t)snprintf(expected, sizeof(expected),
            "[teapot], 42 %s, 0x%" PRIxPTR ", 0xffffffffffffffff, 0xff, 18446744073709551615, ",
            description, address);
        for (size_t i = checkpoint_cnt; i > 0; i--) {
            checkpoint_metadata[i - 1].return_address = UINT64_MAX - i;
            size += (size_t)snprintf(expected + size, sizeof(expected) - size,
                                    "0x%" PRIx64 ", ", UINT64_MAX - i);
        }
        strcpy(expected + size, "\n");
        libcheckpoint_enabled = strcmp(mode, "disabled") != 0;
    }

#ifdef REPORT_TEST_ASAN
    __asan_poison_memory_region(__start_teapot_protected,
                               __stop_teapot_protected - __start_teapot_protected);
#endif
    in_report = true;
    if (full_report)
        report_gadget(description, GADGET_KASPER_CACHE, address, UINT64_MAX, 0xff);
    else
        make_report_call_nop(address);
    in_report = false;
#ifdef REPORT_TEST_ASAN
    __asan_unpoison_memory_region(__start_teapot_protected,
                                 __stop_teapot_protected - __start_teapot_protected);
#endif
    if (full_report) {
        assert(strcmp(output, libcheckpoint_enabled ? expected : "") == 0);
        assert(simulation_statistics.total_bug == (unsigned)libcheckpoint_enabled);
        assert(simulation_statistics.bug_type[GADGET_KASPER_CACHE] == (unsigned)libcheckpoint_enabled);
    }
    if (strcmp(mode, "restore-failure") == 0)
        _exit(99);  /* Returning must not be mistaken for an assertion's SIGABRT. */

    assert(memcmp((void *)address, should_patch ? nop : original,
                  crossing && missing ? 2 : width) == 0);
    assert(protection_at(base) == (PROT_READ | PROT_EXEC));
    assert(protection_at(base + page_size) == (missing ? -1 : PROT_READ | PROT_WRITE));
    for (unsigned i = 0; i < call_count; i++) {
        assert(calls[i].length == page_size);
        assert(calls[i].address == base || (crossing && calls[i].address == base + page_size));
        if (calls[i].address == base + page_size)
            assert(calls[i].protection == (PROT_READ | PROT_WRITE));
    }
    if (should_patch)
        assert(call_count == (crossing ? 4 : 2));
    if (missing_maps || read_error)
        assert(call_count == 0);
    assert(munmap(mapping, 4 * page_size) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    page_size = (size_t)__real_sysconf(_SC_PAGESIZE);
    forbid_allocations = true;
    missing_maps = strcmp(argv[1], "no-maps") == 0;
    read_error = strcmp(argv[1], "read-error") == 0;
    fake_maps = strcmp(argv[1], "split-maps") == 0;
    read_limit = 7;
    read_interrupted = fake_maps;
    write_limit = 3;
    write_interrupted = true;
    if (strcmp(argv[1], "large-page") == 0 && page_size < 65536)
        page_size = 65536;
    if (strcmp(argv[1], "failure") == 0) {
        check_patch(argv[1], false, 1);
        check_patch(argv[1], false, 2);
    } else if (strcmp(argv[1], "restore-failure") == 0) {
        for (unsigned failing_call = 3; failing_call <= 4; failing_call++) {
            pid_t child = fork();
            assert(child >= 0);
            if (child == 0)
                check_patch(argv[1], false, failing_call);
            int status;
            assert(waitpid(child, &status, 0) == child);
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        }
    } else {
        check_patch(argv[1], false, 0);
#if defined(__riscv) && __riscv_xlen == 64
        if (strcmp(argv[1], "isolated") == 0 || strcmp(argv[1], "permissions") == 0 ||
                strcmp(argv[1], "large-page") == 0)
            check_patch(argv[1], true, 0);
#endif
    }
    return 0;
}
