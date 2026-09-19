/*
 * libc.so shim — supplies the bionic version node `LIBC` so that Android-built
 * vendor libraries can be dlopen'd against glibc without patching them.
 *
 * Background
 * ----------
 * The vendor's .so references libc symbols as `puts@LIBC`, `sqrt@LIBC`, etc.
 * Bionic defines a version node named `LIBC`; glibc defines `GLIBC_2.17` and
 * friends, so glibc's loader reports:
 *
 *     version `LIBC' not found (required by ./libthermometry.so)
 *
 * This shim is a real ELF shared object named libc.so whose version script
 * *defines* a `LIBC` node.  glibc's version check therefore succeeds, and the
 * vendor library's versioned references resolve here.
 *
 * Why not patch the vendor .so
 * ----------------------------
 * Rewriting `.gnu.version` in the vendor binary also works up to a point, but
 * it desynchronises `l_versions` (the array glibc indexes during relocation)
 * and segfaults inside dlopen.  Leaving the vendor artifact byte-identical is
 * also better practice: this library is the numerical ground truth for the
 * port, so its hash should stay stable.
 *
 * Why the maths functions are forwarded
 * -------------------------------------
 * `exp`, `pow`, `sqrt` and `sqrtf` must produce glibc's exact results, because
 * the port being validated will link against glibc.  Reimplementing them here
 * would defeat the purpose of the comparison.  They are resolved lazily on
 * first call via dlsym — never during relocation or init, so there is no
 * loader-lock reentrancy risk.
 *
 * Build: see run.sh
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ------------------------------------------------------------------ data --
 * The stack canary.  The vendor library reads this global; any stable value
 * works, since the check only compares it against the copy saved on entry. */
uintptr_t __stack_chk_guard = 0x0badc0de5a5a1234ULL;

/* ------------------------------------------------------- trivial forwards --
 * None of these call any symbol this shim itself defines, so there is no
 * recursion.  They are cheap enough to implement directly. */

void __stack_chk_fail(void)
{
    abort();
}

int puts(const char *s)
{
    if (fputs(s, stdout) < 0)
        return -1;
    return fputc('\n', stdout) < 0 ? -1 : 0;
}

/* Bionic's basename() is the GNU variant: it returns the component after the
 * last '/'.  The vendor only ever passes string literals to it for logging, so
 * a read-only implementation is sufficient. */
char *basename(const char *path)
{
    const char *slash;

    if (path == NULL || *path == '\0')
        return (char *)".";
    slash = strrchr(path, '/');
    return (char *)(slash ? slash + 1 : path);
}

pid_t gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/* The vendor library registers destructors through these.  A test harness does
 * not need them to run, so accept and ignore. */
int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle)
{
    (void)func; (void)arg; (void)dso_handle;
    return 0;
}

void __cxa_finalize(void *dso_handle)
{
    (void)dso_handle;
}

/* ------------------------------------------------------- forwarded maths -- */

static void *libm_handle(void)
{
    static void *h = NULL;
    static int tried = 0;

    if (!tried) {
        tried = 1;
        h = dlopen("libm.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (h == NULL)
            h = dlopen("libm.so.6", RTLD_LAZY);
    }
    return h;
}

static void *must_resolve(const char *name)
{
    void *h = libm_handle();
    void *p = h ? dlsym(h, name) : NULL;

    if (p == NULL) {
        fprintf(stderr, "libc-shim: FATAL: cannot resolve libm symbol '%s'\n", name);
        abort();
    }
    return p;
}

double exp(double x)
{
    static double (*fn)(double);
    if (fn == NULL)
        fn = (double (*)(double))must_resolve("exp");
    return fn(x);
}

double pow(double x, double y)
{
    static double (*fn)(double, double);
    if (fn == NULL)
        fn = (double (*)(double, double))must_resolve("pow");
    return fn(x, y);
}

double sqrt(double x)
{
    static double (*fn)(double);
    if (fn == NULL)
        fn = (double (*)(double))must_resolve("sqrt");
    return fn(x);
}

float sqrtf(float x)
{
    static float (*fn)(float);
    if (fn == NULL)
        fn = (float (*)(float))must_resolve("sqrtf");
    return fn(x);
}
