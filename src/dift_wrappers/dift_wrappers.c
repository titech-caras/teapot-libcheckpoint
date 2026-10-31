#include "dift_support.h"

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *__memcpy_chk(void *dest, const void *src, size_t count, size_t destlen);
extern void *__memset_chk(void *dest, int ch, size_t count, size_t destlen);
extern char *__strncat_chk(char *dest, const char *src, size_t count, size_t destlen);

// glibc's fgets primitive exposes the exact count, including embedded NULs
// and a partial read followed by an error. A post-call string scan cannot.
#ifndef __GLIBC__
#error "The fgets DIFT wrapper requires glibc's counted line reader"
#endif
extern size_t _IO_getline(FILE *stream, char *buffer, size_t size, int delimiter, int extract);

// Teapot's own wrappers: the taint sources, the allocator, the fortified
// copies, memmove, strncat and strncpy. LLVM DFSan's wrappers (dfsan_wrappers.cpp)
// cover the other libc calls Teapot redirects; teapot/dfsan/ in Teapot lists
// which calls go where.

// Taint source: read.
DIFT_WRAPPER(read, ssize_t, int fd, void *buf, size_t count) {
    ssize_t read_size = read(fd, buf, count);
    if (read_size > 0) {
        dift_set_mem_tags(buf, TAG_ATTACKER, read_size);
    }
    dift_reg_tags[DIFT_RET] = 0;
    return read_size;
}

static size_t tagged_fread(void *buffer, size_t size, size_t count, FILE *stream,
                           size_t (*read_fn)(void *, size_t, size_t, FILE *)) {
    dift_reg_tags[DIFT_RET] = 0;
    if (size == 0 || count > SIZE_MAX / size)
        return read_fn(buffer, size, count, stream);
    // A partial final element is copied but omitted from fread's item count.
    size_t read_bytes = read_fn(buffer, 1, size * count, stream);
    if (read_bytes > 0) {
        dift_set_mem_tags(buffer, TAG_ATTACKER, read_bytes);
    }
    return read_bytes / size;
}

// Taint source: fread.
DIFT_WRAPPER(fread, size_t, void *buffer, size_t size, size_t count, FILE *stream) {
    return tagged_fread(buffer, size, count, stream, fread);
}

// Taint source: fread_unlocked.
DIFT_WRAPPER(fread_unlocked, size_t, void *buffer, size_t size, size_t count, FILE *stream) {
    return tagged_fread(buffer, size, count, stream, fread_unlocked);
}

// Taint source: fgets.
DIFT_WRAPPER(fgets, char *, char *str, int n, FILE *stream) {
    if (n <= 1) {
        char *result = fgets(str, n, stream);
        if (result) dift_set_mem_tags(str, 0, 1);
        dift_reg_tags[DIFT_RET] = result ? dift_reg_tags[DIFT_ARG0] : 0;
        return result;
    }

    flockfile(stream);
    // Preserve a previous error, but only a new non-EAGAIN error prevents
    // termination of a nonempty read. This is the glibc fgets contract.
    const int previous_error = stream->_flags & _IO_ERR_SEEN;
    stream->_flags &= ~_IO_ERR_SEEN;
    const size_t count = _IO_getline(stream, str, (size_t)n - 1, '\n', 1);
    const int saved_errno = errno;
    const int success = count && (!ferror(stream) || saved_errno == EAGAIN);
    stream->_flags |= previous_error;
    if (count) dift_set_mem_tags(str, TAG_ATTACKER, count);
    if (success) {
        str[count] = '\0';
        dift_set_mem_tags(str + count, 0, 1);
    }
    funlockfile(stream);
    dift_reg_tags[DIFT_RET] = success ? dift_reg_tags[DIFT_ARG0] : 0;
    errno = saved_errno;
    return success ? str : NULL;
}

// Taint source: getc.
DIFT_WRAPPER(getc, int, FILE *file) {
    dift_reg_tags[DIFT_RET] = TAG_ATTACKER;
    return getc(file);
}

// Taint source: fgetc.
DIFT_WRAPPER(fgetc, int, FILE *file) {
    dift_reg_tags[DIFT_RET] = TAG_ATTACKER;
    return fgetc(file);
}

// Taint source: getchar.
DIFT_WRAPPER(getchar, int) {
    dift_reg_tags[DIFT_RET] = TAG_ATTACKER;
    return getchar();
}

// DFSan's own allocator clears the shadow of new memory and moves it on
// realloc. Here the program uses libc's allocator, so these wrappers do it.
DIFT_WRAPPER(malloc, void *, size_t size) {
    void *addr = malloc(size);
    if (addr != NULL)
        dift_set_mem_tags(addr, 0, malloc_usable_size(addr));
    dift_reg_tags[DIFT_RET] = 0;
    return addr;
}

DIFT_WRAPPER(calloc, void*, size_t num, size_t size) {
    void *addr = calloc(num, size);
    if (addr != NULL)
        dift_set_mem_tags(addr, 0, malloc_usable_size(addr));
    dift_reg_tags[DIFT_RET] = 0;
    return addr;
}

// The old block's usable size is read before realloc frees it. Under ASan an
// invalid pointer is therefore reported by malloc_usable_size, not realloc.
DIFT_WRAPPER(realloc, void *, void *old, size_t size) {
    size_t old_size = old != NULL ? malloc_usable_size(old) : 0;
    void *addr = realloc(old, size);
    if (addr != NULL) {
        size_t new_size = malloc_usable_size(addr);
        size_t kept = old_size < new_size ? old_size : new_size;
        if (addr != old)
            dift_move_mem_tags(addr, old, kept);
        dift_set_mem_tags((char *)addr + kept, 0, new_size - kept);
    }
    dift_reg_tags[DIFT_RET] = 0;
    return addr;
}

// DFSan has no wrappers for the fortified _chk copies, and its strncat and
// strncpy wrappers read past n when the source is unterminated. Its memmove
// wrapper moves the tags with the sanitizer's memmove, which under ASan is
// libc's checked one: on AArch64 ASan rejects the tag addresses and aborts.
DIFT_WRAPPER(memmove, void *, void *dest, const void *src, size_t count) {
    void *result = memmove(dest, src, count);
    dift_move_mem_tags(dest, src, count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(__memcpy_chk, void*, void *dest, const void *src, size_t count, size_t destlen) {
    void *result = __memcpy_chk(dest, src, count, destlen);
    dift_copy_mem_tags(dest, src, count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

static void tag_strncat_result(char *dest, const char *src, size_t n) {
    size_t copied = strnlen(src, n);
    char *append = dest + strlen(dest) - copied;
    dift_copy_mem_tags(append, src, copied + (copied < n));
    // A truncated append writes a new terminator instead of copying src's NUL.
    if (copied == n)
        dift_set_mem_tags(append + copied, 0, 1);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
}

DIFT_WRAPPER(strncat, char *, char *dest, const char *src, size_t n) {
    char *result = strncat(dest, src, n);
    tag_strncat_result(dest, src, n);
    return result;
}

DIFT_WRAPPER(__strncat_chk, char*, char *dest, const char *src, size_t n, size_t s1len) {
    char *result = __strncat_chk(dest, src, n, s1len);
    tag_strncat_result(dest, src, n);
    return result;
}

DIFT_WRAPPER(strncpy, char*, char *dest, const char *src, size_t num) {
    char *result = strncpy(dest, src, num);
    size_t copy_len = strnlen(src, num);
    dift_copy_mem_tags(dest, src, copy_len);
    if (copy_len < num) {
        dift_set_mem_tags(dest + copy_len, 0, num - copy_len);
    }
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(__memset_chk, void*, void *dest, int ch, size_t count, size_t destlen) {
    void *result = __memset_chk(dest, ch, count, destlen);
    dift_set_mem_tags(dest, dift_reg_tags[DIFT_ARG1], count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

