/*
 * libdl.so shim — supplies a `LIBC` version node for the vendor libraries'
 * libdl dependency.
 *
 * Why this exists
 * ---------------
 * libDYTJpegAes.so has *two* verneed records naming `LIBC`: one against
 * libc.so and one against libdl.so (the latter for `dl_iterate_phdr`).  A
 * symlink to the host's libdl.so.2 satisfies the dependency but defines no
 * such version, so the loader reports:
 *
 *     libdl.so: version `LIBC' not found (required by ./libDYTJpegAes.so)
 *
 * libthermometry.so has no libdl dependency, which is why the thermometry
 * harness never needed this file.
 *
 * Build: see build_shims.sh
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
                    void *data)
{
    static int (*fn)(int (*)(struct dl_phdr_info *, size_t, void *), void *);

    if (fn == NULL) {
        /* libdl was merged into glibc's libc in 2.34, so try both. */
        void *h = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (h == NULL)
            h = dlopen("libc.so.6", RTLD_LAZY);
        if (h == NULL)
            h = dlopen("libdl.so.2", RTLD_LAZY);
        fn = h ? (int (*)(int (*)(struct dl_phdr_info *, size_t, void *),
                          void *))dlsym(h, "dl_iterate_phdr") : NULL;
        if (fn == NULL) {
            fprintf(stderr, "libdl-shim: FATAL: cannot resolve dl_iterate_phdr\n");
            abort();
        }
    }
    return fn(cb, data);
}
