/*
 * liblog.so shim — provides the Android logging symbols that the vendor's
 * libthermometry.so needs, so the real ARM/Android-built library can be loaded
 * on a normal glibc host for differential testing.
 *
 * The vendor library calls __android_log_print() with ordinary printf-style
 * format strings.  We route the output to stderr and, if DYT_LOG_FILE is set,
 * also to that file.  Capturing it is worthwhile: thermometryT4Line logs the
 * calibration values it just parsed out of the frame's userArea, which is
 * direct ground truth for the record layout documented in
 * RE Docs/04-usb-protocol.md section 4.5.2.
 *
 * Build:  gcc -shared -fPIC -O2 -o liblog.so liblog.c
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Android log priorities, for reference:
 *   2 VERBOSE  3 DEBUG  4 INFO  5 WARN  6 ERROR  7 ASSERT */

static FILE *log_fp(void)
{
    static FILE *fp = NULL;
    static int tried = 0;

    if (!tried) {
        const char *path = getenv("DYT_LOG_FILE");
        tried = 1;
        if (path && *path)
            fp = fopen(path, "a");
    }
    return fp;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int n;
    FILE *fp = log_fp();

    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (fp) {
        va_start(ap, fmt);
        fprintf(fp, "[prio=%d tag=%s] ", prio, tag ? tag : "?");
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fflush(fp);
    }
    (void)tag;
    return n;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    int n = vfprintf(stderr, fmt, ap);
    FILE *fp = log_fp();

    if (fp) {
        fprintf(fp, "[prio=%d tag=%s] ", prio, tag ? tag : "?");
        va_list ap2;
        va_copy(ap2, ap);
        vfprintf(fp, fmt, ap2);
        va_end(ap2);
        fflush(fp);
    }
    return n;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    FILE *fp = log_fp();
    int n = fprintf(stderr, "%s", text);

    if (fp) {
        fprintf(fp, "[prio=%d tag=%s] %s", prio, tag ? tag : "?", text);
        fflush(fp);
    }
    return n;
}

/* Bionic exposes gettid() as a libc function; glibc gained it in 2.30, but
 * provide it unconditionally so the shim works on older hosts too.  If glibc
 * already defines it, our definition simply takes precedence in this object. */
pid_t gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}
