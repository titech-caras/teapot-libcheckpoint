/* Private, allocation-free Linux mapping-footprint inspection.  This file is
 * included by shadow_mapping.c, not exported as a runtime interface. */
#include <fcntl.h>
#include <sys/statfs.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_MASK
#define MAP_HUGE_MASK 0x3f
#endif
#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif
#ifndef ARCH_GET_UNTAG_MASK
#define ARCH_GET_UNTAG_MASK 0x4001
#endif
#ifndef PR_GET_TAGGED_ADDR_CTRL
#define PR_GET_TAGGED_ADDR_CTRL 56
#endif
#ifndef PR_TAGGED_ADDR_ENABLE
#define PR_TAGGED_ADDR_ENABLE 1UL
#endif
#ifndef PR_PMLEN_SHIFT
#define PR_PMLEN_SHIFT 24
#define PR_PMLEN_MASK (0x7fUL << PR_PMLEN_SHIFT)
#endif

enum mapping_address_kind { MAPPING_UNTAGGED, MAPPING_FIXED_DESTINATION };

static int mapping_power_of_two(uintptr_t value) {
    return value && !(value & (value-1));
}

#if defined(__riscv) && __riscv_xlen == 64
static uintptr_t mapping_riscv_untagged(uintptr_t address, unsigned pmlen) {
    if (!pmlen) return address;
    uintptr_t mask = UINTPTR_MAX >> pmlen;
    uintptr_t low = address & mask;
    return (low & ((mask >> 1)+1)) ? low | ~mask : low;
}
#endif

/* Linux deliberately does not untag mmap addresses or mremap's new_address.
 * Keep that distinction even when a tagged argument would normalize to an
 * owned range: on the current ABI that argument is instead kernel-invalid. */
static int mapping_address(uintptr_t address, enum mapping_address_kind kind,
                           uintptr_t *normalized) {
    if (kind == MAPPING_FIXED_DESTINATION) {
#if defined(__aarch64__)
        /* These two arguments changed ABI in Linux 5.6. A version string
         * cannot prove which behavior an older/backported kernel implements.
         * Refuse only a tagged positive address whose two interpretations
         * differ; ordinary canonical addresses need no kernel-version gate. */
        if (!(address & (UINT64_C(1) << 55)) &&
            (address & UINT64_C(0xff00000000000000))) return 0;
#endif
        *normalized = address;
        return 1;
    }
#if defined(__x86_64__)
    uintptr_t mask = UINTPTR_MAX;
    long result;
    do { result = syscall(SYS_arch_prctl, ARCH_GET_UNTAG_MASK, &mask); }
    while (result < 0 && errno == EINTR);
    if (result < 0) {
        /* Match the fault backend's hardware proof. A denied/unknown query,
         * even EINVAL, is not proof that a LAM-capable CPU is unmasked. */
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_count(7,1,&eax,&ebx,&ecx,&edx) && (eax & (1U<<26))) return 0;
        mask = UINTPTR_MAX;
    }
    if (mask != UINTPTR_MAX && mask != UINT64_C(0x81ffffffffffffff) &&
        mask != UINT64_C(0x8000ffffffffffff)) return 0;
    *normalized = address & mask;
#elif defined(__aarch64__)
    /* arm64 untagged_addr is addr & sign_extend64(addr,55), not an
     * unconditional top-byte mask, and this VM API class needs no prctl opt-in. */
    *normalized = (address & (UINT64_C(1) << 55)) ? address :
                  address & UINT64_C(0x00ffffffffffffff);
#elif defined(__riscv) && __riscv_xlen == 64
    long mode;
    do { mode = syscall(SYS_prctl, PR_GET_TAGGED_ADDR_CTRL, 0UL, 0UL, 0UL, 0UL); }
    while (mode < 0 && errno == EINTR);
    if (mode < 0) {
        /* The supported RV64 pointer-masking ISA has exactly PMLEN 0/7/16.
         * A query error establishes no active mode. Only their per-address
         * identity intersection is nevertheless an exact transform proof. */
        if (mapping_riscv_untagged(address,7) != address ||
            mapping_riscv_untagged(address,16) != address) return 0;
        *normalized = address;
        return 1;
    }
    if ((unsigned long)mode & ~(PR_PMLEN_MASK | PR_TAGGED_ADDR_ENABLE)) return 0;
    unsigned pmlen = ((unsigned long)mode & PR_PMLEN_MASK) >> PR_PMLEN_SHIFT;
    if (pmlen != 0 && pmlen != 7 && pmlen != 16) return 0;
    if ((mode & PR_TAGGED_ADDR_ENABLE) && !pmlen) return 0;
    if (!(mode & PR_TAGGED_ADDR_ENABLE)) pmlen = 0;
    /* RV64 MMU untagged_addr sign-extends the remaining virtual address.
     * PR_GET returns the actual PMLEN, unlike PR_SET's requested minimum. */
    *normalized = mapping_riscv_untagged(address,pmlen);
#else
    (void)address;
    (void)normalized;
    return 0;
#endif
    return 1;
}

/* A bounded streaming reader, with no FILE, heap, string-library call, or
 * application mapping wrapper. Long path suffixes are consumed, not copied. */
struct mapping_reader {
    int fd;
    size_t next, used, total;
    unsigned char bytes[512];
};

static int mapping_open(struct mapping_reader *reader, const char *path) {
    long fd;
    do { fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return 0;
    reader->fd = (int)fd;
    reader->next = reader->used = reader->total = 0;
    return 1;
}

static void mapping_close(struct mapping_reader *reader) {
    /* On Linux even EINTR has already released this descriptor; never retry. */
    (void)syscall(SYS_close, reader->fd);
}

static int mapping_line(struct mapping_reader *reader, char *line, size_t capacity,
                        int *truncated) {
    size_t length = 0;
    int any = 0;
    *truncated = 0;
    for (;;) {
        if (reader->next == reader->used) {
            if (reader->total >= 16U*1024U*1024U) return -1;
            long count;
            do { count = syscall(SYS_read, reader->fd, reader->bytes, sizeof(reader->bytes)); }
            while (count < 0 && errno == EINTR);
            if (count < 0) return -1;
            if (!count) { line[length] = '\0'; return any; }
            reader->next = 0;
            reader->used = (size_t)count;
            reader->total += (size_t)count;
        }
        unsigned char ch = reader->bytes[reader->next++];
        any = 1;
        if (ch == '\n') { line[length] = '\0'; return 1; }
        if (!ch) return -1;
        if (length+1 < capacity) line[length++] = (char)ch;
        else *truncated = 1;
    }
}

static const char *mapping_prefix(const char *text, const char *prefix) {
    while (*prefix) if (*text++ != *prefix++) return NULL;
    return text;
}

static const char *mapping_number(const char *text, unsigned base, uintptr_t *value) {
    uintptr_t number = 0;
    int any = 0;
    for (;;) {
        unsigned digit;
        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text-'0');
        else if (*text >= 'a' && *text <= 'f') digit = (unsigned)(*text-'a')+10;
        else if (*text >= 'A' && *text <= 'F') digit = (unsigned)(*text-'A')+10;
        else break;
        if (digit >= base) break;
        if (number > (UINTPTR_MAX-digit)/base) return NULL;
        number = number*base+digit;
        ++text;
        any = 1;
    }
    if (!any) return NULL;
    *value = number;
    return text;
}

static int mapping_kilobytes(const char *text, uintptr_t *bytes) {
    while (*text == ' ' || *text == '\t') ++text;
    uintptr_t kb;
    text = mapping_number(text,10,&kb);
    if (!text || kb > UINTPTR_MAX/1024) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    text = mapping_prefix(text,"kB");
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text) return 0;
    *bytes = kb*1024;
    return mapping_power_of_two(*bytes);
}

static int mapping_default_huge_page(uintptr_t *page) {
    struct mapping_reader reader;
    if (!mapping_open(&reader,"/proc/meminfo")) return 0;
    char line[160];
    int truncated, result = 0;
    while (mapping_line(&reader,line,sizeof(line),&truncated) > 0) {
        const char *value = mapping_prefix(line,"Hugepagesize:");
        if (!value) continue;
        result = !truncated && mapping_kilobytes(value,page);
        break;
    }
    mapping_close(&reader);
    return result;
}

static int mapping_mmap_granule(int flags, int fd, uintptr_t *granule) {
    uintptr_t base_page = (uintptr_t)getpagesize();
    if (!mapping_power_of_two(base_page)) return 0;
    *granule = base_page;
    if (!(flags & MAP_ANONYMOUS)) {
        struct statfs status;
        long result;
        do { result = syscall(SYS_fstatfs,fd,&status); }
        while (result < 0 && errno == EINTR);
        /* An invalid descriptor cannot install a mapping. Preserve the real
         * mmap failure rather than inventing an unknown-footprint refusal. */
        if (result < 0) return errno == EBADF;
        if ((unsigned long)status.f_type != HUGETLBFS_MAGIC) return 1;
        if (status.f_bsize <= 0) return 0;
        *granule = (uintptr_t)status.f_bsize;
    } else if (flags & MAP_HUGETLB) {
        unsigned shift = ((unsigned)flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK;
        if (shift) *granule = (uintptr_t)1 << shift;
        else if (!mapping_default_huge_page(granule)) return 0;
    }
    return *granule >= base_page && mapping_power_of_two(*granule);
}

static int mapping_vma_header(const char *line, uintptr_t *start, uintptr_t *end) {
    const char *tail = mapping_number(line,16,start);
    if (!tail || *tail++ != '-') return 0;
    tail = mapping_number(tail,16,end);
    return tail && *tail == ' ' && *start < *end;
}

static int mapping_hugetlb_flag(const char *text) {
    while (*text) {
        while (*text == ' ' || *text == '\t') ++text;
        const char *token = text;
        while (*text && *text != ' ' && *text != '\t') ++text;
        if (text-token == 2 && token[0] == 'h' && token[1] == 't') return 1;
    }
    return 0;
}

static int mapping_mremap_granule(uintptr_t address, uintptr_t *granule) {
    uintptr_t base_page = (uintptr_t)getpagesize();
    if (!mapping_power_of_two(base_page)) return 0;
    struct mapping_reader reader;
    if (!mapping_open(&reader,"/proc/self/smaps")) return 0;
    char line[160];
    int truncated, state = 0, result = 0, line_status, saw_header = 0;
    uintptr_t kernel_page = 0, previous_end = 0;
    while ((line_status = mapping_line(&reader,line,sizeof(line),&truncated)) > 0) {
        uintptr_t start, end;
        if (mapping_vma_header(line,&start,&end)) {
            if (state) break; /* Matching VMA had incomplete metadata. */
            if (saw_header && start < previous_end) break;
            saw_header = 1;
            previous_end = end;
            if (address < start) { *granule = base_page; result = 1; break; }
            state = address < end;
            continue;
        }
        if (!saw_header) break; /* Empty/malformed input is not an absent VMA. */
        if (!state) continue;
        const char *value = mapping_prefix(line,"KernelPageSize:");
        if (value) {
            if (truncated || kernel_page || !mapping_kilobytes(value,&kernel_page)) break;
            continue;
        }
        value = mapping_prefix(line,"VmFlags:");
        if (!value) continue;
        if (truncated) break;
        /* THP is not hugetlb: it must not inflate mremap's syscall footprint. */
        *granule = mapping_hugetlb_flag(value) ? kernel_page : base_page;
        result = *granule >= base_page && mapping_power_of_two(*granule);
        break;
    }
    /* A complete scan proving no source VMA means no huge-page rounding.
     * The original call still supplies its normal return value and errno. */
    if (line_status == 0 && saw_header && !state) { *granule = base_page; result = 1; }
    mapping_close(&reader);
    return result;
}
