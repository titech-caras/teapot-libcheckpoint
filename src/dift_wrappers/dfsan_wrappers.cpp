// LLVM DFSan's custom libc wrappers (third_party/llvm-dfsan, unmodified) over
// Teapot's shadow, in one translation unit: the upstream wrappers, the DFSan
// runtime services they call, and the bridges Teapot redirects calls to. The
// build keeps only the bridges global (cmake/DfsanWrappers.cmake), so nothing
// here can bind to a sanitizer runtime linked into the same program.
//
// Built with -funsigned-char: upstream's comparison and search loops use plain
// char, which agrees with libc's bytes only when char is unsigned, as it
// already is on AArch64 and RISC-V.

// Keep libc's memcpy/memmove/memset; upstream would otherwise alias them to
// the sanitizer's own copies. Keep ISO C strto*: C++ implies _GNU_SOURCE,
// which selects glibc's C23 strtol, and that also accepts a 0b prefix.
#define SANITIZER_COMMON_NO_REDEFINE_BUILTINS
#include <features.h>
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0

#include "dfsan/dfsan_custom.cpp"

#include <sys/syscall.h>

using namespace __sanitizer;

// Labels are Teapot tags, combined by OR. Zero-initialized flags leave
// strict_data_dependencies off: a scan or comparison result carries the
// tags of the bytes it read, as in a static binary's instrumented libc.
namespace __dfsan {
Flags flags_data;
}

extern "C" {
dfsan_label dfsan_union(dfsan_label l1, dfsan_label l2) { return l1 | l2; }

void dfsan_set_label(dfsan_label label, void *addr, uptr size) {
  dift_set_mem_tags(addr, label, size);
}

dfsan_label dfsan_read_label(const void *addr, uptr size) {
  const dfsan_label *shadow = __dfsan::shadow_for(addr);
  dfsan_label label = 0;
  for (uptr i = 0; i < size; i++) label |= shadow[i];
  return label;
}

void dfsan_mem_shadow_transfer(void *dst, const void *src, uptr len) {
  dift_move_mem_tags(dst, src, len);
}

void *__sanitizer_internal_memcpy(void *dest, const void *src, uptr n) {
  return memcpy(dest, src, n);
}

void *__sanitizer_internal_memmove(void *dest, const void *src, uptr n) {
  return memmove(dest, src, n);
}

void *__sanitizer_internal_memset(void *s, int c, uptr n) {
  return memset(s, c, n);
}
}  // extern "C"

namespace __sanitizer {
atomic_uint32_t current_verbosity;

void Report(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

void NORETURN CheckFailed(const char *file, int line, const char *cond, u64 v1,
                          u64 v2) {
  fprintf(stderr, "DFSan wrapper check failed: %s:%d %s (%llu, %llu)\n", file,
          line, cond, (unsigned long long)v1, (unsigned long long)v2);
  abort();
}

void StaticSpinMutex::LockSlow() {
  while (!TryLock()) sched_yield();
}

void SetSigProcMask(__sanitizer_sigset_t *set, __sanitizer_sigset_t *oldset) {
  CHECK_EQ(0, pthread_sigmask(SIG_SETMASK, (sigset_t *)set, (sigset_t *)oldset));
}

ScopedBlockSignals::ScopedBlockSignals(__sanitizer_sigset_t *copy) {
  sigset_t all;
  sigfillset(&all);
  CHECK_EQ(0, pthread_sigmask(SIG_SETMASK, &all, (sigset_t *)&saved_));
  if (copy) internal_memcpy(copy, &saved_, sizeof(saved_));
}

ScopedBlockSignals::~ScopedBlockSignals() { SetSigProcMask(&saved_, nullptr); }

ThreadID GetTid() { return syscall(SYS_gettid); }
}  // namespace __sanitizer

// Origins, argument TLS, DFSan's threads and fork locks, and dlopen's
// region walk: only wrappers that are never bridged reach these.
extern "C" {
void dfsan_clear_arg_tls(uptr, uptr) { __builtin_trap(); }
void dfsan_clear_thread_local_state() { __builtin_trap(); }
void dfsan_set_arg_tls(uptr, dfsan_label) { __builtin_trap(); }
void dfsan_set_arg_origin_tls(uptr, dfsan_origin) { __builtin_trap(); }
dfsan_origin dfsan_read_origin_of_first_taint(const void *, uptr) { __builtin_trap(); }
void dfsan_set_label_origin(dfsan_label, dfsan_origin, void *, uptr) { __builtin_trap(); }
void dfsan_mem_origin_transfer(const void *, const void *, uptr) { __builtin_trap(); }
}

namespace __sanitizer {
void ForEachMappedRegion(link_map *, void (*)(const void *, uptr)) { __builtin_trap(); }
void AdjustStackSize(void *) { __builtin_trap(); }
void StackDepotLockBeforeFork() { __builtin_trap(); }
void StackDepotUnlockAfterFork(bool) { __builtin_trap(); }
}  // namespace __sanitizer

namespace __dfsan {
void ChainedOriginDepotLockBeforeFork() { __builtin_trap(); }
void ChainedOriginDepotUnlockAfterFork(bool) { __builtin_trap(); }
DFsanThread *DFsanThread::Create(thread_callback_t, void *, bool) { __builtin_trap(); }
void DFsanThread::Init() { __builtin_trap(); }
thread_return_t DFsanThread::ThreadStart() { __builtin_trap(); }
DFsanThread *GetCurrentThread() { __builtin_trap(); }
void SetCurrentThread(DFsanThread *) { __builtin_trap(); }
}  // namespace __dfsan

// name__dift_wrapper__, which Teapot calls in place of name: run DFSan's
// __dfsw_name with the argument registers' tags, and store the result label
// as the return register's tag. Arguments and results are integers or
// pointers, so argument i is in integer argument register i.
#define DFSW_LABEL(i) dift_reg_tags[DIFT_ARG##i]
#define DFSW_RETURN(ret_t, call)                                               \
  dfsan_label ret_label = 0;                                                   \
  ret_t ret = call;                                                            \
  dift_reg_tags[DIFT_RET] = ret_label;                                         \
  return ret
#define DFSW_BRIDGE1(ret_t, name, t0)                                          \
  extern "C" ret_t name##__dift_wrapper__(t0 a0) {                             \
    DFSW_RETURN(ret_t, __dfsw_##name(a0, DFSW_LABEL(0), &ret_label));          \
  }
#define DFSW_BRIDGE2(ret_t, name, t0, t1)                                      \
  extern "C" ret_t name##__dift_wrapper__(t0 a0, t1 a1) {                      \
    DFSW_RETURN(ret_t, __dfsw_##name(a0, a1, DFSW_LABEL(0), DFSW_LABEL(1),     \
                                     &ret_label));                             \
  }
#define DFSW_BRIDGE3(ret_t, name, t0, t1, t2)                                  \
  extern "C" ret_t name##__dift_wrapper__(t0 a0, t1 a1, t2 a2) {               \
    DFSW_RETURN(ret_t, __dfsw_##name(a0, a1, a2, DFSW_LABEL(0), DFSW_LABEL(1), \
                                     DFSW_LABEL(2), &ret_label));              \
  }

DFSW_BRIDGE3(void *, memcpy, void *, const void *, size_t)
DFSW_BRIDGE3(void *, memmove, void *, const void *, size_t)
DFSW_BRIDGE3(void *, memset, void *, int, size_t)
DFSW_BRIDGE2(char *, strcpy, char *, const char *)
DFSW_BRIDGE2(char *, strcat, char *, const char *)
DFSW_BRIDGE1(char *, strdup, const char *)
DFSW_BRIDGE1(size_t, strlen, const char *)
DFSW_BRIDGE2(char *, strrchr, char *, int)
DFSW_BRIDGE2(char *, strpbrk, const char *, const char *)
DFSW_BRIDGE2(char *, strstr, char *, char *)
DFSW_BRIDGE3(void *, memchr, void *, int, size_t)
DFSW_BRIDGE3(long, strtol, const char *, char **, int)
DFSW_BRIDGE3(long long, strtoll, const char *, char **, int)
DFSW_BRIDGE3(unsigned long, strtoul, const char *, char **, int)
DFSW_BRIDGE3(unsigned long long, strtoull, const char *, char **, int)
DFSW_BRIDGE3(int, inet_pton, int, const char *, void *)

// Upstream computes these results itself. Take them from libc instead, so
// ASan's checks and a fuzzer's comparison hooks still see each call, and use
// upstream's wrapper for the label alone.
#define DFSW_LABEL_ONLY(ret_t, name, params, args, labels)                     \
  extern "C" ret_t name##__dift_wrapper__ params {                             \
    ret_t ret = (ret_t)name args;                                              \
    dfsan_label ret_label = 0;                                                 \
    __dfsw_##name labels;                                                      \
    dift_reg_tags[DIFT_RET] = ret_label;                                       \
    return ret;                                                                \
  }
#define DFSW_COMPARE2(name)                                                    \
  DFSW_LABEL_ONLY(int, name, (const char *a0, const char *a1), (a0, a1),       \
                  (a0, a1, DFSW_LABEL(0), DFSW_LABEL(1), &ret_label))
#define DFSW_COMPARE3(name, t)                                                 \
  DFSW_LABEL_ONLY(int, name, (t a0, t a1, size_t a2), (a0, a1, a2),            \
                  (a0, a1, a2, DFSW_LABEL(0), DFSW_LABEL(1), DFSW_LABEL(2),    \
                   &ret_label))
DFSW_COMPARE3(memcmp, const void *)
DFSW_COMPARE3(bcmp, const void *)
DFSW_COMPARE2(strcmp)
DFSW_COMPARE3(strncmp, const char *)
DFSW_COMPARE2(strcasecmp)
DFSW_COMPARE3(strncasecmp, const char *)
// Upstream's search loop compares s[i] with c itself; libc compares with
// (char)c. With unsigned char, passing (unsigned char)c gives the same bytes.
DFSW_LABEL_ONLY(char *, strchr, (const char *a0, int a1), (a0, a1),
                (a0, (unsigned char)a1, DFSW_LABEL(0), DFSW_LABEL(1), &ret_label))

// Not in DFSan's list. glibc's atoi is (int)strtol(nptr, NULL, 10).
extern "C" int atoi__dift_wrapper__(const char *nptr) {
  DFSW_RETURN(int, (int)__dfsw_strtol(nptr, nullptr, 10, DFSW_LABEL(0), 0, 0,
                                      &ret_label));
}

// Upstream aliases __isoc23_strto* to the ISO wrappers, which would drop the
// C23 0b prefix. Call the C23 functions with upstream's label rule instead.
// Weak: only a program that imports one reaches it, and its libc has it.
#define DFSW_ISOC23_STRTO(ret_t, name)                                         \
  extern "C" ret_t __isoc23_##name(const char *, char **, int)                 \
      __attribute__((weak));                                                   \
  extern "C" ret_t __isoc23_##name##__dift_wrapper__(const char *nptr,         \
                                                     char **endptr, int base) { \
    char *tmp_endptr;                                                          \
    dfsan_label ret_label = 0;                                                 \
    ret_t ret = dfsan_strtol_impl(__isoc23_##name, nptr, endptr, base,         \
                                  &tmp_endptr);                                \
    dfsan_strtolong_label(nptr, tmp_endptr, DFSW_LABEL(2), &ret_label);        \
    dift_reg_tags[DIFT_RET] = ret_label;                                       \
    return ret;                                                                \
  }
DFSW_ISOC23_STRTO(long, strtol)
DFSW_ISOC23_STRTO(long long, strtoll)
DFSW_ISOC23_STRTO(unsigned long, strtoul)
DFSW_ISOC23_STRTO(unsigned long long, strtoull)
