#include "dift_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

extern void *__memcpy_chk(void *dest, const void *src, size_t count, size_t destlen);
extern void *__memset_chk(void *dest, int ch, size_t count, size_t destlen);
extern char *__strncat_chk(char *dest, const char *src, size_t count, size_t destlen);

// glibc's fgets primitive exposes the exact count, including embedded NULs
// and a partial read followed by an error. A post-call string scan cannot.
#ifndef __GLIBC__
#error "The fgets DIFT wrapper requires glibc's counted line reader"
#endif
extern size_t _IO_getline(FILE *stream, char *buffer, size_t size, int delimiter, int extract);

// TODO: eventually move this into an independent project and make the interface compatible with dfsan

// Taint source: read.
DIFT_WRAPPER(read, ssize_t, int fd, void *buf, size_t count) {
    ssize_t read_size = read(fd, buf, count);
    if (read_size > 0) {
        dift_set_mem_tags(buf, TAG_ATTACKER, read_size);
    }
    return read_size;
}

static size_t tagged_fread(void *buffer, size_t size, size_t count, FILE *stream,
                           size_t (*read_fn)(void *, size_t, size_t, FILE *)) {
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

DIFT_WRAPPER(calloc, void*, size_t num, size_t size) {
    void *addr = calloc(num, size);
    if (addr != NULL)
        dift_set_mem_tags(addr, 0, num * size);
    return addr;
}

DIFT_WRAPPER(atoi, int, const char *str) {
    dift_reg_tags[DIFT_RET] = DIFT_MEM_TAG(str);
    return atoi(str);
}

DIFT_WRAPPER(strtol, long int, const char *nptr, char **endptr, int base) {
    dift_reg_tags[DIFT_RET] = DIFT_MEM_TAG(nptr);
    return strtol(nptr, endptr, base);
}

DIFT_WRAPPER(strtoul, unsigned long int, const char *nptr, char **endptr, int base) {
    dift_reg_tags[DIFT_RET] = DIFT_MEM_TAG(nptr);
    return strtoul(nptr, endptr, base);
}


DIFT_WRAPPER(strlen, size_t, const char *str) {
    dift_reg_tags[DIFT_RET] = DIFT_MEM_TAG(str);
    return strlen(str);
}

DIFT_WRAPPER(strdup, char*, const char *string) {
    char *result = strdup(string);
    if (result != NULL)
        dift_copy_mem_tags(result, string, strlen(string) + 1);
    return result;
}

DIFT_WRAPPER(memcpy, void*, void *dest, const void *src, size_t count) {
    void *result = memcpy(dest, src, count);
    dift_copy_mem_tags(dest, src, count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(__memcpy_chk, void*, void *dest, const void *src, size_t count, size_t destlen) {
    void *result = __memcpy_chk(dest, src, count, destlen);
    dift_copy_mem_tags(dest, src, count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(memmove, void*, void *dest, const void *src, size_t count) {
    void *result = memmove(dest, src, count);
    dift_move_mem_tags(dest, src, count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(strcpy, char*, char *dest, const char *src) {
    char *result = strcpy(dest, src);
    dift_copy_mem_tags(dest, src, strlen(src) + 1);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(strcat, char*, char *dest, const char *src) {
    char *result = strcat(dest, src);
    size_t len = strlen(src);
    dift_copy_mem_tags(dest + strlen(dest) - len, src, len + 1);
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

DIFT_WRAPPER(strncat, char*, char *dest, const char *src, size_t n) {
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

DIFT_WRAPPER(memset, void*, void *dest, int ch, size_t count) {
    void *result = memset(dest, ch, count);
    dift_set_mem_tags(dest, dift_reg_tags[DIFT_ARG1], count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(__memset_chk, void*, void *dest, int ch, size_t count, size_t destlen) {
    void *result = __memset_chk(dest, ch, count, destlen);
    dift_set_mem_tags(dest, dift_reg_tags[DIFT_ARG1], count);
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return result;
}

DIFT_WRAPPER(strtok_r, char *, char *str, const char *delims, char **saveptr) {
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return strtok_r(str, delims, saveptr);
}

DIFT_WRAPPER(strtok, char *, char *str, const char *delims) {
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return strtok(str, delims);
}

DIFT_WRAPPER(strstr, char *, const char *haystack, const char *needle) {
    dift_reg_tags[DIFT_RET] = dift_reg_tags[DIFT_ARG0];
    return strstr(haystack, needle);
}

DIFT_WRAPPER(inet_pton, int, int af, const char *src, void *dst) {
    int result = inet_pton(af, src, dst);
    if (result == 1)
        dift_set_mem_tags(dst, DIFT_MEM_TAG(src),
                          af == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr));
    return result;
}
