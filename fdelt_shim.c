#include <unistd.h>

#define FD_LIMIT 1024
#define BITS_PER_WORD 64

static unsigned long suppressed;

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_long(char *p, long v)
{
    char digits[20];
    int n = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

    if (v < 0)
        *p++ = '-';
    do {
        digits[n++] = '0' + u % 10;
        u /= 10;
    } while (u);
    while (n)
        *p++ = digits[--n];
    return p;
}

static void report(long fd, unsigned long count)
{
    char line[96];
    char *p = line;

    p = put(p, "fdelt-shim: fd ");
    p = put_long(p, fd);
    p = put(p, " is outside the fd_set range, suppressed abort #");
    p = put_long(p, (long)count);
    *p++ = '\n';
    (void)!write(STDERR_FILENO, line, p - line);
}

long __fdelt_chk(long fd)
{
    if (fd >= 0 && fd < FD_LIMIT)
        return fd / BITS_PER_WORD;

    unsigned long count = __atomic_add_fetch(&suppressed, 1, __ATOMIC_RELAXED);

    if ((count & (count - 1)) == 0)
        report(fd, count);

    /* Word 0 keeps the caller's bit test inside its own fd_set. */
    return 0;
}

long __fdelt_warn(long fd) __attribute__((alias("__fdelt_chk")));
