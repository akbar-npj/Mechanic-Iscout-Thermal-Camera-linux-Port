/*
 * libc.so shim — supplies the bionic version nodes `LIBC` and `LIBC_N` so that
 * Android-built vendor libraries can be dlopen'd against glibc without
 * patching them.
 *
 * Background
 * ----------
 * The vendor's .so references libc symbols as `puts@LIBC`, `malloc@LIBC`, etc.
 * Bionic defines version nodes named `LIBC` and `LIBC_N`; glibc defines
 * `GLIBC_2.17` and friends, so glibc's loader reports:
 *
 *     version `LIBC' not found (required by ./libthermometry.so)
 *
 * This shim is a real ELF shared object named libc.so whose version script
 * *defines* both nodes.  glibc's version check therefore succeeds, and the
 * vendor library's versioned references resolve here.
 *
 * `LIBC_N` is bionic's node for the FORTIFY variants (`__fread_chk` and
 * friends) added in later NDK releases; libDYTJpegAes.so (NDK r23b) uses it,
 * while libthermometry.so predates it and uses `LIBC` only.
 *
 * Why not patch the vendor .so
 * ----------------------------
 * Rewriting `.gnu.version` in the vendor binary also works up to a point, but
 * it desynchronises `l_versions` (the array glibc indexes during relocation)
 * and segfaults inside dlopen.  Leaving the vendor artifact byte-identical is
 * also better practice: these libraries are the numerical ground truth for the
 * port, so their hashes should stay stable.
 *
 * How symbols are provided
 * ------------------------
 *  - **libc and maths functions are forwarded to glibc.**  `exp`, `pow`,
 *    `sqrt`, `memcpy`, `fopen` and the rest must behave exactly as glibc's,
 *    because the port being validated links against glibc.  Reimplementing
 *    them here would defeat the purpose of the comparison.  They resolve
 *    lazily on first call via dlsym — never during relocation or init, so
 *    there is no loader-lock reentrancy risk.
 *  - **FORTIFY wrappers** perform bionic's bound check and then forward, so a
 *    genuine overflow still aborts rather than being silently tolerated.
 *  - **Data symbols** (`__sF`, `_ctype_`) cannot be forwarded — they are
 *    addresses, not calls — so real storage is defined here.  `_ctype_` is a
 *    correct bionic-layout ASCII table; `__sF` is bionic's stdio array, for
 *    which a plausibly-sized zero block is enough, because the vendor logs
 *    through __android_log_print rather than stdio and nothing on the paths
 *    these harnesses exercise reads it.
 *
 * Scope: this shim covers every undefined symbol the two vendor libraries
 * declare.  The harnesses dlopen with RTLD_LAZY, so a symbol that is imported
 * but never called need not resolve — but everything reachable is here.
 *
 * Build: see build_shims.sh
 */

#define _GNU_SOURCE

/* glibc >= 2.38 redirects the strtol/wcstol families to `__isoc23_*` when C23
 * support is on — and `_GNU_SOURCE` turns it on.  The redirect is attached to
 * the *declaration* as an `__asm__` label, so a definition of `strtol` here
 * would emit `__isoc23_strtol` and the vendor's `strtol@LIBC` would not
 * resolve.  glibc's own strtol.c disables the redirect the same way: include
 * features.h first (so the second, guarded include inside stdlib.h is a no-op),
 * then override the flag. */
#include <features.h>
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0

#include <dlfcn.h>
#include <dirent.h>
#include <locale.h>
#include <stdarg.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* A few of the symbols below are declared by glibc as `_Generic` macros rather
 * than plain functions.  The shim *defines* those symbols, so the macros have
 * to go first or the definition is rewritten into nonsense. */
#undef memchr
#undef wmemchr
#undef bsearch
#undef getc
#undef putchar
#undef feof
#undef ferror
#undef stat
#undef lstat
#undef fstat
#undef getcwd

/* ------------------------------------------------------------------ data --
 * The stack canary.  The vendor library reads this global; any stable value
 * works, since the check only compares it against the copy saved on entry. */
uintptr_t __stack_chk_guard = 0x0badc0de5a5a1234ULL;

/* Bionic's stdio array: `stdout` is `&__sF[1]`, `stderr` is `&__sF[2]`.  The
 * vendor .so has a relocation against it, so storage must exist; see the
 * header comment for why a zero block is acceptable here. */
char __sF[3 * 128];

/* Bionic's ctype table: one byte per character, indexed by c + 1, with bit
 * flags (_U upper, _L lower, _D digit, _S space, _P punct, _C control, _X hex,
 * _B blank).  Defined properly rather than zeroed, so that a call into an
 * is*() macro cannot silently change behaviour. */
const unsigned char _ctype_[257] = {
    0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xa8, 0x28,
    0x28, 0x28, 0x28, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x88, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
    0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
    0x42, 0x42, 0x42, 0x42, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

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

    if (*path == '\0')
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

/* Bionic's errno accessor.  glibc's errno is a macro over __errno_location();
 * the vendor calls __errno() because that is how bionic declares it. */
int *__errno(void)
{
    extern int *__errno_location(void);
    return __errno_location();
}

/* Registration hooks the vendor's C++ runtime calls.  Nothing here needs them
 * to fire, and pretending they did would be worse than ignoring them. */
int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void *dso_handle)
{
    (void)prepare; (void)parent; (void)child; (void)dso_handle;
    return 0;
}

void android_set_abort_message(const char *msg)
{
    if (msg)
        fprintf(stderr, "libc-shim: abort message: %s\n", msg);
}

/* ---------------------------------------------------- forwarded to glibc --
 * Resolved lazily on first call.  `libc.so.6` carries the C library and (since
 * glibc 2.34) the pthread symbols; the others are fallbacks so the shim also
 * works on an older host. */

static void *glibc_sym(const char *name)
{
    static const char *const libs[] = {
        "libc.so.6", "libpthread.so.0", "libm.so.6", NULL
    };
    void *p = NULL;
    int   i;

    for (i = 0; libs[i] != NULL && p == NULL; i++) {
        void *h = dlopen(libs[i], RTLD_LAZY | RTLD_NOLOAD);
        if (h == NULL)
            h = dlopen(libs[i], RTLD_LAZY);
        if (h != NULL)
            p = dlsym(h, name);
    }
    if (p == NULL) {
        fprintf(stderr, "libc-shim: FATAL: cannot resolve '%s'\n", name);
        abort();
    }
    return p;
}

#define FWD(ret, name, params, args)                    \
    ret name params                                     \
    {                                                   \
        static ret (*fn) params;                        \
        if (fn == NULL)                                 \
            fn = (ret (*) params)glibc_sym(#name);      \
        return fn args;                                 \
    }

FWD(double, exp,   (double x),                    (x))
FWD(double, pow,   (double x, double y),          (x, y))
FWD(double, sqrt,  (double x),                    (x))
FWD(float,  sqrtf, (float x),                     (x))

FWD(void *,  malloc,   (size_t n),                 (n))
FWD(void *,  calloc,   (size_t n, size_t s),       (n, s))
FWD(void *,  realloc,  (void *p, size_t n),        (p, n))
FWD(void,    free,     (void *p),                  (p))
FWD(int,     posix_memalign, (void **p, size_t a, size_t n), (p, a, n))

/* abort() is declared noreturn, so it cannot go through FWD (which ends in a
 * `return`).  Forward to glibc's abort, which raises SIGABRT on the host. */
void abort(void)
{
    static void (*fn)(void);
    if (fn == NULL)
        fn = (void (*)(void))glibc_sym("abort");
    fn();
    __builtin_unreachable();
}

/* exit() is likewise declared noreturn. */
void exit(int status)
{
    static void (*fn)(int);
    if (fn == NULL)
        fn = (void (*)(int))glibc_sym("exit");
    fn(status);
    __builtin_unreachable();
}

FWD(void *,  memcpy,   (void *d, const void *s, size_t n),       (d, s, n))
FWD(void *,  memmove,  (void *d, const void *s, size_t n),       (d, s, n))
FWD(void *,  memset,   (void *d, int c, size_t n),               (d, c, n))
FWD(int,     memcmp,   (const void *a, const void *b, size_t n), (a, b, n))
FWD(void *,  memchr,   (const void *s, int c, size_t n),         (s, c, n))

FWD(size_t,  strlen,   (const char *s),            (s))
FWD(int,     strcmp,   (const char *a, const char *b), (a, b))
FWD(char *,  strcat,   (char *d, const char *s),   (d, s))
FWD(char *,  strcpy,   (char *d, const char *s),   (d, s))

FWD(int,     atoi,     (const char *s),            (s))
FWD(long,    strtol,   (const char *s, char **e, int b),  (s, e, b))
FWD(long long, strtoll, (const char *s, char **e, int b), (s, e, b))
FWD(unsigned long, strtoul, (const char *s, char **e, int b), (s, e, b))
FWD(unsigned long long, strtoull, (const char *s, char **e, int b), (s, e, b))
FWD(float,   strtof,   (const char *s, char **e),  (s, e))
FWD(double,  strtod,   (const char *s, char **e),  (s, e))
FWD(long double, strtold, (const char *s, char **e), (s, e))

FWD(FILE *,  fopen,    (const char *p, const char *m), (p, m))
FWD(int,     fclose,   (FILE *f),                  (f))
FWD(size_t,  fread,    (void *p, size_t s, size_t n, FILE *f), (p, s, n, f))
FWD(size_t,  fwrite,   (const void *p, size_t s, size_t n, FILE *f), (p, s, n, f))
FWD(int,     fseek,    (FILE *f, long o, int w),   (f, o, w))
FWD(long,    ftell,    (FILE *f),                  (f))
FWD(int,     fflush,   (FILE *f),                  (f))
FWD(int,     fputc,    (int c, FILE *f),           (c, f))
FWD(int,     fputs,    (const char *s, FILE *f),   (s, f))
FWD(int,     vfprintf, (FILE *f, const char *fmt, va_list ap), (f, fmt, ap))
FWD(int,     vprintf,  (const char *fmt, va_list ap),          (fmt, ap))

FWD(size_t,  wcslen,   (const wchar_t *s),         (s))
FWD(wchar_t *, wmemcpy, (wchar_t *d, const wchar_t *s, size_t n), (d, s, n))
FWD(wchar_t *, wmemmove, (wchar_t *d, const wchar_t *s, size_t n), (d, s, n))
FWD(wchar_t *, wmemset, (wchar_t *d, wchar_t c, size_t n), (d, c, n))
FWD(int,     wmemcmp,  (const wchar_t *a, const wchar_t *b, size_t n), (a, b, n))
FWD(wchar_t *, wmemchr, (const wchar_t *s, wchar_t c, size_t n), (s, c, n))
FWD(long,    wcstol,   (const wchar_t *s, wchar_t **e, int b), (s, e, b))
FWD(long long, wcstoll, (const wchar_t *s, wchar_t **e, int b), (s, e, b))
FWD(unsigned long, wcstoul, (const wchar_t *s, wchar_t **e, int b), (s, e, b))
FWD(unsigned long long, wcstoull, (const wchar_t *s, wchar_t **e, int b), (s, e, b))
FWD(float,   wcstof,   (const wchar_t *s, wchar_t **e), (s, e))
FWD(double,  wcstod,   (const wchar_t *s, wchar_t **e), (s, e))
FWD(long double, wcstold, (const wchar_t *s, wchar_t **e), (s, e))

FWD(int, pthread_key_create,  (pthread_key_t *k, void (*d)(void *)), (k, d))
FWD(int, pthread_key_delete,  (pthread_key_t k),          (k))
FWD(void *, pthread_getspecific, (pthread_key_t k),        (k))
FWD(int, pthread_setspecific, (pthread_key_t k, const void *v), (k, v))
FWD(int, pthread_mutex_lock,  (pthread_mutex_t *m),        (m))
FWD(int, pthread_mutex_unlock,(pthread_mutex_t *m),        (m))
FWD(int, pthread_once,        (pthread_once_t *o, void (*f)(void)), (o, f))
FWD(int, pthread_rwlock_rdlock, (pthread_rwlock_t *l),     (l))
FWD(int, pthread_rwlock_wrlock, (pthread_rwlock_t *l),     (l))
FWD(int, pthread_rwlock_unlock, (pthread_rwlock_t *l),     (l))

/* ------------------------------------------------------- added for MNN --
 * The MNN stack imports far more of libc than the two vendor libraries the
 * shim was originally written for: libMNN.so, libMNN_Express.so,
 * libmnnmodel.so and the APK's libc++_shared.so together name ~137 symbols the
 * shim did not define (the whole locale and dirent surface, most of pthread, a
 * dozen maths functions).  They are all ordinary glibc functions, so they
 * forward exactly like the rest.
 *
 * Only a fraction is actually reached -- the rest are imported and never
 * called, which is why prep_vendor_so.py clears BIND_NOW so they need not
 * resolve at load time.  The thread pool is the exception: MNN's CPU backend
 * really does spawn workers and wait on condition variables, so these were the
 * first lazy-binding failures the harness hit. */

FWD(int, pthread_create,        (pthread_t *t, const pthread_attr_t *a,
                                 void *(*f)(void *), void *x), (t, a, f, x))
FWD(int, pthread_join,          (pthread_t t, void **r),       (t, r))
FWD(int, pthread_detach,        (pthread_t t),                 (t))
FWD(int, pthread_equal,         (pthread_t a, pthread_t b),    (a, b))
FWD(pthread_t, pthread_self,    (void),                        ())
FWD(int, pthread_mutex_init,    (pthread_mutex_t *m, const pthread_mutexattr_t *a), (m, a))
FWD(int, pthread_mutex_destroy, (pthread_mutex_t *m),          (m))
FWD(int, pthread_mutex_trylock, (pthread_mutex_t *m),          (m))
FWD(int, pthread_mutexattr_init,    (pthread_mutexattr_t *a),  (a))
FWD(int, pthread_mutexattr_destroy, (pthread_mutexattr_t *a),  (a))
FWD(int, pthread_mutexattr_settype, (pthread_mutexattr_t *a, int t), (a, t))
FWD(int, pthread_cond_init,     (pthread_cond_t *c, const pthread_condattr_t *a), (c, a))
FWD(int, pthread_cond_destroy,  (pthread_cond_t *c),           (c))
FWD(int, pthread_cond_wait,     (pthread_cond_t *c, pthread_mutex_t *m), (c, m))
FWD(int, pthread_cond_timedwait,(pthread_cond_t *c, pthread_mutex_t *m,
                                 const struct timespec *t),    (c, m, t))
FWD(int, pthread_cond_signal,   (pthread_cond_t *c),           (c))
FWD(int, pthread_cond_broadcast,(pthread_cond_t *c),           (c))
FWD(int, sched_yield,           (void),                        ())

/* Time.  MNN::Timer reads the clock on every session. */
FWD(int, clock_gettime, (clockid_t c, struct timespec *t),      (c, t))
FWD(int, gettimeofday,  (struct timeval *t, void *tz),          (t, tz))
FWD(int, nanosleep,     (const struct timespec *r, struct timespec *l), (r, l))

/* Assorted libc the runtime and libc++ reach for. */
FWD(unsigned long, getauxval, (unsigned long t),                (t))
FWD(long,     sysconf,   (int n),                               (n))
FWD(char *,   getenv,    (const char *n),                       (n))
FWD(char *,   strdup,    (const char *s),                       (s))
FWD(int,      strncmp,   (const char *a, const char *b, size_t n), (a, b, n))
FWD(size_t,   strnlen,   (const char *s, size_t n),             (s, n))
FWD(void *,   bsearch,   (const void *k, const void *b, size_t n, size_t s,
                          int (*c)(const void *, const void *)), (k, b, n, s, c))

/* The float maths family.  These return in v0, so the prototype has to be
 * exact -- unlike the integer forwards, a wrong return type would be silently
 * wrong rather than merely unreachable. */
FWD(float,  acosf,  (float x),                    (x))
FWD(float,  acoshf, (float x),                    (x))
FWD(float,  asinf,  (float x),                    (x))
FWD(float,  asinhf, (float x),                    (x))
FWD(float,  atanf,  (float x),                    (x))
FWD(float,  atan2f, (float y, float x),           (y, x))
FWD(float,  atanhf, (float x),                    (x))
FWD(float,  cosf,   (float x),                    (x))
FWD(float,  coshf,  (float x),                    (x))
FWD(double, erf,    (double x),                   (x))
FWD(float,  erff,   (float x),                    (x))
FWD(float,  erfcf,  (float x),                    (x))
FWD(float,  expf,   (float x),                    (x))
FWD(float,  fmodf,  (float x, float y),           (x, y))
FWD(double, frexp,  (double x, int *e),           (x, e))
FWD(double, log,    (double x),                   (x))
FWD(float,  logf,   (float x),                    (x))
FWD(float,  log1pf, (float x),                    (x))
FWD(float,  powf,   (float x, float y),           (x, y))
FWD(float,  sinf,   (float x),                    (x))
FWD(float,  sinhf,  (float x),                    (x))
FWD(float,  tanf,   (float x),                    (x))
FWD(float,  tanhf,  (float x),                    (x))

/* Files, directories and metadata.  libMNN.so probes the filesystem when it
 * builds its CPU topology, and libc++_shared.so touches stat/realpath on the
 * locale path. */
FWD(int,     close,     (int fd),                               (fd))
FWD(int,     chdir,     (const char *p),                        (p))
FWD(char *,  getcwd,    (char *b, size_t n),                    (b, n))
FWD(int,     fstat,     (int fd, struct stat *s),               (fd, s))
FWD(int,     stat,      (const char *p, struct stat *s),        (p, s))
FWD(int,     lstat,     (const char *p, struct stat *s),        (p, s))
FWD(int,     mkdir,     (const char *p, mode_t m),              (p, m))
FWD(int,     remove,    (const char *p),                        (p))
FWD(int,     rename,    (const char *a, const char *b),         (a, b))
FWD(int,     link,      (const char *a, const char *b),         (a, b))
FWD(int,     symlink,   (const char *a, const char *b),         (a, b))
FWD(ssize_t, readlink,  (const char *p, char *b, size_t n),     (p, b, n))
FWD(char *,  realpath,  (const char *p, char *r),               (p, r))
FWD(int,     truncate,  (const char *p, off_t n),               (p, n))
FWD(int,     ftruncate, (int fd, off_t n),                      (fd, n))
FWD(int,     fchmod,    (int fd, mode_t m),                     (fd, m))
FWD(int,     fchmodat,  (int d, const char *p, mode_t m, int f),(d, p, m, f))
FWD(int,     utimensat, (int d, const char *p, const struct timespec t[2], int f), (d, p, t, f))
FWD(ssize_t, sendfile,  (int o, int i, off_t *off, size_t n),   (o, i, off, n))
FWD(long,    pathconf,  (const char *p, int n),                 (p, n))
FWD(int,     statvfs,   (const char *p, struct statvfs *s),     (p, s))
FWD(DIR *,   opendir,   (const char *p),                        (p))
FWD(struct dirent *, readdir, (DIR *d),                         (d))
FWD(int,     closedir,  (DIR *d),                               (d))

/* NOTE: dlopen and dlsym are deliberately NOT defined here, even though the
 * MNN libraries import them.  `glibc_sym()` below is built on dlopen/dlsym,
 * so defining them in this file would make every forwarded call recurse into
 * itself until the stack is exhausted.  Leaving them undefined is safe: with
 * BIND_NOW cleared (see prep_vendor_so.py, fix 4) an imported-but-uncalled
 * symbol never has to resolve, and nothing on the inference path opens a
 * shared object.  A lazy-binding failure here would be the signal that some
 * backend loader does need them, and it would need a non-recursive route. */

/* stdio the runtime still touches. */
FWD(int,   feof,    (FILE *f),                        (f))
FWD(int,   ferror,  (FILE *f),                        (f))
FWD(char *,fgets,   (char *b, int n, FILE *f),        (b, n, f))
FWD(int,   getc,    (FILE *f),                        (f))
FWD(int,   ungetc,  (int c, FILE *f),                 (c, f))
FWD(int,   putchar, (int c),                          (c))

/* bionic's strerror_r returns int (XSI semantics); glibc's default GNU variant
 * returns char*.  Bind to the XSI symbol so the contract matches bionic.
 * glibc declares `strerror_r` with the GNU prototype, so the definition is
 * emitted under the symbol name via an asm label rather than by declaring a
 * conflicting C function -- the same technique used for the strtol family. */
int shim_strerror_r(int errnum, char *buf, size_t buflen) __asm__("strerror_r");

int shim_strerror_r(int errnum, char *buf, size_t buflen)
{
    static int (*fn)(int, char *, size_t);
    if (fn == NULL)
        fn = (int (*)(int, char *, size_t))glibc_sym("__xpg_strerror_r");
    return fn(errnum, buf, buflen);
}

/* Locale.  libc++_shared.so drags in the whole <locale> surface whether or not
 * anything asks for it, because its facets reference these at load time. */
FWD(locale_t, newlocale,  (int m, const char *n, locale_t b),   (m, n, b))
FWD(locale_t, uselocale,  (locale_t l),                         (l))
FWD(void,     freelocale, (locale_t l),                         (l))
FWD(char *,   setlocale,  (int c, const char *l),               (c, l))
FWD(struct lconv *, localeconv, (void),                         ())
FWD(int,    strcoll_l,  (const char *a, const char *b, locale_t l),  (a, b, l))
FWD(size_t, strxfrm_l,  (char *d, const char *s, size_t n, locale_t l), (d, s, n, l))
FWD(int,    wcscoll_l,  (const wchar_t *a, const wchar_t *b, locale_t l), (a, b, l))
FWD(size_t, wcsxfrm_l,  (wchar_t *d, const wchar_t *s, size_t n, locale_t l), (d, s, n, l))
FWD(size_t, strftime_l, (char *d, size_t n, const char *f, const struct tm *t, locale_t l), (d, n, f, t, l))
FWD(int,    isdigit_l,  (int c, locale_t l),                    (c, l))
FWD(int,    islower_l,  (int c, locale_t l),                    (c, l))
FWD(int,    isupper_l,  (int c, locale_t l),                    (c, l))
FWD(int,    isxdigit_l, (int c, locale_t l),                    (c, l))
FWD(int,    tolower_l,  (int c, locale_t l),                    (c, l))
FWD(int,    toupper_l,  (int c, locale_t l),                    (c, l))
FWD(long long, strtoll_l,  (const char *s, char **e, int b, locale_t l), (s, e, b, l))
FWD(unsigned long long, strtoull_l, (const char *s, char **e, int b, locale_t l), (s, e, b, l))
FWD(long double, strtold_l, (const char *s, char **e, locale_t l), (s, e, l))

/* Wide-character conversion and classification. */
FWD(wint_t, btowc,   (int c),                                   (c))
FWD(int,    wctob,   (wint_t c),                                (c))
FWD(int,    mbtowc,  (wchar_t *p, const char *s, size_t n),     (p, s, n))
FWD(size_t, mbrtowc, (wchar_t *p, const char *s, size_t n, mbstate_t *st), (p, s, n, st))
FWD(size_t, mbrlen,  (const char *s, size_t n, mbstate_t *st),  (s, n, st))
FWD(size_t, wcrtomb, (char *s, wchar_t c, mbstate_t *st),       (s, c, st))
FWD(size_t, mbsrtowcs,  (wchar_t *d, const char **s, size_t n, mbstate_t *st), (d, s, n, st))
FWD(size_t, mbsnrtowcs, (wchar_t *d, const char **s, size_t sn, size_t n, mbstate_t *st), (d, s, sn, n, st))
FWD(size_t, wcsnrtombs, (char *d, const wchar_t **s, size_t sn, size_t n, mbstate_t *st), (d, s, sn, n, st))
FWD(int,    iswalpha_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswblank_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswcntrl_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswdigit_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswlower_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswprint_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswpunct_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswspace_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswupper_l,  (wint_t c, locale_t l),                (c, l))
FWD(int,    iswxdigit_l, (wint_t c, locale_t l),                (c, l))
FWD(wint_t, towlower_l,  (wint_t c, locale_t l),                (c, l))
FWD(wint_t, towupper_l,  (wint_t c, locale_t l),                (c, l))

/* sincosf writes both results through pointers. */
void sincosf(float x, float *s, float *c)
{
    static void (*fn)(float, float *, float *);
    if (fn == NULL)
        fn = (void (*)(float, float *, float *))glibc_sym("sincosf");
    fn(x, s, c);
}

/* --------------------------------------------------------------- variadic --
 * These cannot go through FWD, which has no way to forward a va_list; each
 * wraps the matching v-function instead. */

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *dst, size_t n, const char *fmt, ...)
{
    extern int vsnprintf(char *, size_t, const char *, va_list);
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}

int swprintf(wchar_t *dst, size_t n, const wchar_t *fmt, ...)
{
    extern int vswprintf(wchar_t *, size_t, const wchar_t *, va_list);
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vswprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    static int (*fn)(char **, const char *, va_list);
    if (fn == NULL)
        fn = (int (*)(char **, const char *, va_list))glibc_sym("vasprintf");
    return fn(out, fmt, ap);
}

void openlog(const char *ident, int option, int facility)
{
    static void (*fn)(const char *, int, int);
    if (fn == NULL)
        fn = (void (*)(const char *, int, int))glibc_sym("openlog");
    fn(ident, option, facility);
}

void closelog(void)
{
    static void (*fn)(void);
    if (fn == NULL)
        fn = (void (*)(void))glibc_sym("closelog");
    fn();
}

void syslog(int prio, const char *fmt, ...)
{
    extern void vsyslog(int, const char *, va_list);
    va_list ap;

    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);        /* glibc's syslog is variadic; forward via v */
    va_end(ap);
}

int printf(const char *fmt, ...)
{
    extern int vprintf(const char *, va_list);
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);          /* writes to stdout, no FILE* reference */
    va_end(ap);
    return n;
}

/* The scanf family is redirected by glibc's headers to `__isoc99_*` (the same
 * kind of `__REDIRECT` the strtol note at the top of this file describes), so
 * a plain definition would emit `__isoc99_sscanf` instead of `sscanf`.  Emit
 * the symbols under asm labels, and bind the v-forms to glibc's real
 * `__isoc99_*` entry points. */
int shim_vsscanf(const char *s, const char *fmt, va_list ap) __asm__("vsscanf");
int shim_vfscanf(FILE *f, const char *fmt, va_list ap) __asm__("vfscanf");
int shim_sscanf(const char *s, const char *fmt, ...) __asm__("sscanf");
int shim_fscanf(FILE *f, const char *fmt, ...) __asm__("fscanf");

int shim_vsscanf(const char *s, const char *fmt, va_list ap)
{
    static int (*fn)(const char *, const char *, va_list);
    if (fn == NULL)
        fn = (int (*)(const char *, const char *, va_list))
             glibc_sym("__isoc99_vsscanf");
    return fn(s, fmt, ap);
}

int shim_vfscanf(FILE *f, const char *fmt, va_list ap)
{
    static int (*fn)(FILE *, const char *, va_list);
    if (fn == NULL)
        fn = (int (*)(FILE *, const char *, va_list))
             glibc_sym("__isoc99_vfscanf");
    return fn(f, fmt, ap);
}

int shim_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = shim_vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}

int shim_fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = shim_vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}
/* syscall(2) is variadic and glibc's version takes the same six arguments, so
 * forward all six.  Reading more varargs than the caller supplied is formally
 * undefined, but on aarch64 the callee's prologue spills x1-x7 to the register
 * save area, so the extra reads yield register garbage that glibc ignores --
 * it dispatches on the syscall number alone.  The MNN path uses syscall for
 * gettid and a few futex operations. */
long syscall(long number, ...)
{
    static long (*fn)(long, long, long, long, long, long, long);
    va_list ap;
    long    a1, a2, a3, a4, a5, a6;

    if (fn == NULL)
        fn = (long (*)(long, long, long, long, long, long, long))
             glibc_sym("syscall");

    va_start(ap, number);
    a1 = va_arg(ap, long);
    a2 = va_arg(ap, long);
    a3 = va_arg(ap, long);
    a4 = va_arg(ap, long);
    a5 = va_arg(ap, long);
    a6 = va_arg(ap, long);
    va_end(ap);
    return fn(number, a1, a2, a3, a4, a5, a6);
}

/* ------------------------------------------------- bionic-only entry points --
 * These have no glibc equivalent under the same name. */

/* bionic's `MB_CUR_MAX` accessor; glibc exports the same thing.  glibc's
 * prototype returns size_t, so this definition must match. */
size_t __ctype_get_mb_cur_max(void)
{
    static size_t (*fn)(void);
    if (fn == NULL)
        fn = (size_t (*)(void))glibc_sym("__ctype_get_mb_cur_max");
    return fn();
}

/* Android system properties do not exist here.  Reporting "not found" (0) is
 * the honest answer; nothing on the inference path depends on a property. */
int __system_property_get(const char *name, char *value)
{
    (void)name;
    if (value)
        value[0] = '\0';
    return 0;
}

/* bionic's checked read(): verify the bound, then read. */
ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size)
{
    if (count > buf_size)
        abort();
    return read(fd, buf, count);
}

int __open_2(const char *path, int flags)
{
    static int (*fn)(const char *, int);
    if (fn == NULL)
        fn = (int (*)(const char *, int))glibc_sym("open");
    return fn(path, flags);
}

/* bionic's vsprintf into a caller-sized buffer; the bound is the point. */
int __vsprintf_chk(char *dst, int flag, size_t dst_len,
                   const char *fmt, va_list ap)
{
    (void)flag;
    return vsnprintf(dst, dst_len, fmt, ap);
}

/* --------------------------------------------------------------- FORTIFY ---
 * Bionic's checked variants (the f* ones live in version node LIBC_N).  The
 * bound check is real: a violation aborts, exactly as bionic would, so a
 * genuine overflow in the vendor library cannot pass unnoticed. */

static void chk_fail(const char *what)
{
    fprintf(stderr, "libc-shim: FATAL: %s buffer overflow detected\n", what);
    abort();
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len)
{
    if (n > dst_len) chk_fail("__memcpy_chk");
    return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len)
{
    if (n > dst_len) chk_fail("__memmove_chk");
    return memmove(dst, src, n);
}

void *__memset_chk(void *dst, int c, size_t n, size_t dst_len)
{
    if (n > dst_len) chk_fail("__memset_chk");
    return memset(dst, c, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dst_len)
{
    if (strlen(dst) + strlen(src) + 1 > dst_len) chk_fail("__strcat_chk");
    return strcat(dst, src);
}

char *__strcpy_chk(char *dst, const char *src, size_t dst_len)
{
    if (strlen(src) + 1 > dst_len) chk_fail("__strcpy_chk");
    return strcpy(dst, src);
}

size_t __strlen_chk(const char *s, size_t maxlen)
{
    size_t n = strlen(s);
    if (n >= maxlen) chk_fail("__strlen_chk");
    return n;
}

int __vsnprintf_chk(char *dst, size_t n, int flags, size_t dst_len,
                    const char *fmt, va_list ap)
{
    extern int vsnprintf(char *, size_t, const char *, va_list);
    (void)flags;
    if (n > dst_len) chk_fail("__vsnprintf_chk");
    return vsnprintf(dst, n, fmt, ap);
}

size_t __fread_chk(void *dst, size_t dst_len, size_t size, size_t count,
                   FILE *fp)
{
    if (size != 0 && count > dst_len / size) chk_fail("__fread_chk");
    return fread(dst, size, count, fp);
}

size_t __fwrite_chk(const void *src, size_t src_len, size_t size,
                    size_t count, FILE *fp)
{
    if (size != 0 && count > src_len / size) chk_fail("__fwrite_chk");
    return fwrite(src, size, count, fp);
}
