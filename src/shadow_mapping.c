#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include "shadow_mapping.h"
#include "config.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#include "shadow_mapping_footprint.h"

struct teapot_shadow_range teapot_shadow_registry[TEAPOT_SHADOW_REGISTRY_CAPACITY]
    LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
uint64_t teapot_shadow_registry_count LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
uint64_t teapot_shadow_mapping_ready LIBCHECKPOINT_PROTECTED_SECTION_ALIGNED(8);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_shadow_registry);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_shadow_registry_count);
LIBCHECKPOINT_ASSERT_PROTECTED(teapot_shadow_mapping_ready);

/* The bss section is absent in a standalone wrapper fixture. Real runtime
 * archives define both; the registry itself makes protected nonempty. */
extern char __start_teapot_protected[], __stop_teapot_protected[];
extern char __start_teapot_protected_bss[] __attribute__((weak));
extern char __stop_teapot_protected_bss[] __attribute__((weak));

static __attribute__((noreturn)) void mapping_abort(const char *message, size_t length) {
    /* No allocation, formatting, stdio, or intercepted memory routine before
     * abort, including when an application calls a wrapper during simulation. */
    size_t offset = 0;
    while (offset < length) {
        long done = syscall(SYS_write, STDERR_FILENO, message+offset, length-offset);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) break;
        offset += (size_t)done;
    }
    abort();
}

static __attribute__((noreturn)) void reject_mapping(void) {
    static const char message[] =
        "Teapot: mapping API refuses runtime-owned shadow/protected memory\n";
    mapping_abort(message,sizeof(message)-1);
}

static __attribute__((noreturn)) void reject_unknown_mapping(void) {
    static const char message[] =
        "Teapot: mapping API refuses unknown mapping footprint/address transform\n";
    mapping_abort(message,sizeof(message)-1);
}

struct mapping_signal_guard {
    uint64_t previous;
    int active;
};

static struct mapping_signal_guard mapping_block_signals(int needed) {
    struct mapping_signal_guard guard;
    guard.previous = 0;
    guard.active = TEAPOT_SHADOW_MAPPING_ENFORCEMENT && needed;
    if (guard.active) {
        uint64_t all = UINT64_MAX;
        /* Linux's kernel sigset is 64 bits on all three supported 64-bit
         * ABIs. The kernel excludes SIGKILL/SIGSTOP itself. Do not use glibc's
         * larger sigset_t, or expose a handler between classification and the
         * original mapping operation. The runtime remains single-threaded. */
        if (syscall(SYS_rt_sigprocmask,SIG_BLOCK,&all,&guard.previous,sizeof(all)))
            reject_unknown_mapping();
    }
    return guard;
}

static void mapping_restore_signals(struct mapping_signal_guard *guard, int result_errno) {
    if (guard->active &&
        syscall(SYS_rt_sigprocmask,SIG_SETMASK,&guard->previous,NULL,sizeof(guard->previous))) {
        static const char message[] = "Teapot: cannot restore mapping signal mask\n";
        mapping_abort(message,sizeof(message)-1);
    }
    /* A deferred handler can execute during restoration and change errno. */
    errno = result_errno;
}

static int mmap_needs_signal_guard(int flags) {
    if (!(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) return 0;
    if (!(flags & MAP_ANONYMOUS)) return 1;
    return (flags & MAP_HUGETLB) && !(((unsigned)flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
}

void teapot_shadow_register_owned(uintptr_t start, uintptr_t end, int rw_dift) {
    if (!TEAPOT_SHADOW_MAPPING_ENFORCEMENT || start >= end) return;
    uint64_t count = teapot_shadow_registry_count;
    if (count >= TEAPOT_SHADOW_REGISTRY_CAPACITY) reject_mapping();
    teapot_shadow_registry[count] = (struct teapot_shadow_range){start,end,!!rw_dift};
    /* Publish a complete record, never a count naming uninitialized bounds. */
    __atomic_store_n(&teapot_shadow_registry_count, count+1, __ATOMIC_RELEASE);
}

static int overlap(uintptr_t lo, uintptr_t hi, uintptr_t start, uintptr_t end) {
    return start < end && lo < end && start < hi;
}

static int touches_owned(uintptr_t begin, size_t length, uintptr_t granule) {
    if (!TEAPOT_SHADOW_MAPPING_ENFORCEMENT || !length) return 0;
    uintptr_t page = (uintptr_t)getpagesize();
    if (!mapping_power_of_two(page) || granule < page || !mapping_power_of_two(granule))
        reject_unknown_mapping();
    uintptr_t low = begin & ~(page-1);
    uintptr_t high;
    /* Saturate instead of wrapping. Invalid non-overlapping ranges still go
     * to the original libc call; overlapping requests fail conservatively. */
    if (granule > page && length > UINTPTR_MAX-(granule-1))
        high = UINTPTR_MAX;
    else {
        uintptr_t rounded = granule == page ? length : (length+granule-1) & ~(granule-1);
        if (rounded > UINTPTR_MAX-begin || begin+rounded > UINTPTR_MAX-(page-1))
            high = UINTPTR_MAX;
        else high = (begin+rounded+page-1) & ~(page-1);
    }
    if (overlap(low,high,(uintptr_t)__start_teapot_protected,(uintptr_t)__stop_teapot_protected) ||
        overlap(low,high,(uintptr_t)__start_teapot_protected_bss,(uintptr_t)__stop_teapot_protected_bss))
        return 1;
    uint64_t count = __atomic_load_n(&teapot_shadow_registry_count,__ATOMIC_ACQUIRE);
    if (count > TEAPOT_SHADOW_REGISTRY_CAPACITY) reject_mapping();
    for (uint64_t i=0;i<count;++i)
        if (overlap(low,high,teapot_shadow_registry[i].start,teapot_shadow_registry[i].end))
            return 1;
    return 0;
}

static uintptr_t checked_address(const void *address, enum mapping_address_kind kind) {
    uintptr_t normalized;
    if (!mapping_address((uintptr_t)address,kind,&normalized)) reject_unknown_mapping();
    return normalized;
}

static void check_basic_mapping(const void *address, size_t length) {
    if (!TEAPOT_SHADOW_MAPPING_ENFORCEMENT || !length) return;
    uintptr_t begin = checked_address(address,MAPPING_UNTAGGED);
    if (touches_owned(begin,length,(uintptr_t)getpagesize())) reject_mapping();
}

static void check_fixed_mapping(const void *address, size_t length, int flags, int fd) {
    if (!TEAPOT_SHADOW_MAPPING_ENFORCEMENT || !length ||
        !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) return;
    uintptr_t begin = checked_address(address,MAPPING_FIXED_DESTINATION);
    /* Refuse obvious intersections without requiring any procfs metadata. */
    if (touches_owned(begin,length,(uintptr_t)getpagesize())) reject_mapping();
    uintptr_t granule;
    if (!mapping_mmap_granule(flags,fd,&granule)) reject_unknown_mapping();
    if (touches_owned(begin,length,granule)) reject_mapping();
}

static void check_mremap_tail(uintptr_t begin, uintptr_t destination,
                              size_t old_size, size_t new_size, int flags) {
    uintptr_t page = (uintptr_t)getpagesize();
    if (!(flags & MREMAP_FIXED) || (begin & (page-1)) ||
        old_size > UINTPTR_MAX-(page-1) || new_size > UINTPTR_MAX-(page-1)) return;
    uintptr_t old_length = (old_size+page-1) & ~(page-1);
    uintptr_t new_length = (new_size+page-1) & ~(page-1);
    if (!old_length || old_length != new_length || old_length > UINTPTR_MAX-begin) return;
    /* Recent Linux move-only mremap can span several VMAs. A final partial
     * hugetlb segment can extend both footprints even if the first VMA was
     * ordinary. Earlier kernels use first-VMA rounding, already checked by
     * the caller. Protect the union without guessing the kernel version. */
    uintptr_t end = begin+old_length, granule;
    if (!mapping_mremap_granule(end-1,&granule)) reject_unknown_mapping();
    uintptr_t tail = end & (granule-1);
    if (!tail) return;
    uintptr_t padding = granule-tail;
    uintptr_t extent = old_length > UINTPTR_MAX-padding ? UINTPTR_MAX : old_length+padding;
    if (touches_owned(begin,extent,page) || touches_owned(destination,extent,page)) reject_mapping();
}

/* These symbols are bound only by Teapot's identity-based application import
 * rewrite. This runtime object is never rewritten: its original libc symbols
 * remain original, so internal report/BTI/MTE permission changes do not recurse
 * or acquire a blanket exemption usable by application calls. */
int mprotect__teapot_wrapper__(void *address, size_t length, int protection) {
    int saved = errno;
    check_basic_mapping(address,length);
    errno = saved;
    return mprotect(address,length,protection);
}

int pkey_mprotect__teapot_wrapper__(void *address, size_t length, int protection, int key) {
    int saved = errno;
    check_basic_mapping(address,length);
    errno = saved;
    return pkey_mprotect(address,length,protection,key);
}

int munmap__teapot_wrapper__(void *address, size_t length) {
    int saved = errno;
    check_basic_mapping(address,length);
    errno = saved;
    return munmap(address,length);
}

void *mremap__teapot_wrapper__(void *old, size_t old_size, size_t new_size, int flags, ...) {
    int saved = errno;
    struct mapping_signal_guard guard = mapping_block_signals(1);
    void *destination = NULL;
    if (flags & MREMAP_FIXED) {
        va_list args;
        va_start(args,flags);
        destination = va_arg(args,void *);
        va_end(args);
    }
    if (TEAPOT_SHADOW_MAPPING_ENFORCEMENT) {
        uintptr_t begin = checked_address(old,MAPPING_UNTAGGED);
        uintptr_t target = 0, page = (uintptr_t)getpagesize(), granule;
        if (touches_owned(begin,old_size ? old_size : 1,page)) reject_mapping();
        if (flags & MREMAP_FIXED) {
            target = checked_address(destination,MAPPING_FIXED_DESTINATION);
            if (touches_owned(target,new_size,page)) reject_mapping();
        }
        if (!mapping_mremap_granule(begin,&granule)) reject_unknown_mapping();
        if (touches_owned(begin,old_size ? old_size : 1,granule) ||
            ((flags & MREMAP_FIXED) && touches_owned(target,new_size,granule))) reject_mapping();
        check_mremap_tail(begin,target,old_size,new_size,flags);
    }
    errno = saved;
    void *result = (flags & MREMAP_FIXED) ? mremap(old,old_size,new_size,flags,destination) :
                                         mremap(old,old_size,new_size,flags);
    mapping_restore_signals(&guard,errno);
    return result;
}

void *mmap__teapot_wrapper__(void *address, size_t length, int protection, int flags,
                            int fd, off_t offset) {
    int saved = errno;
    struct mapping_signal_guard guard = mapping_block_signals(mmap_needs_signal_guard(flags));
    check_fixed_mapping(address,length,flags,fd);
    errno = saved;
    void *result = mmap(address,length,protection,flags,fd,offset);
    mapping_restore_signals(&guard,errno);
    return result;
}

void *mmap64__teapot_wrapper__(void *address, size_t length, int protection, int flags,
                              int fd, off64_t offset) {
    int saved = errno;
    struct mapping_signal_guard guard = mapping_block_signals(mmap_needs_signal_guard(flags));
    check_fixed_mapping(address,length,flags,fd);
    errno = saved;
    void *result = mmap64(address,length,protection,flags,fd,offset);
    mapping_restore_signals(&guard,errno);
    return result;
}
