/*
 * probe.c — minimal feasibility probe: can the vendor's Android-built
 * libthermometry.so be dlopen'd on this glibc host, and do its entry points
 * resolve?
 *
 * usage: probe <path-to-libthermometry.so>
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

static const char *names[] = {
    "GetTempEvn", "CalcFixRaw", "InitTempParam", "GetFix", "distanceFix",
    "thermometryT", "thermometryT4Line",
    "thermometrySearch", "thermometrySearchSingle", "thermometrySearchCMM",
    NULL
};

int main(int argc, char **argv)
{
    void *h;
    int i, missing = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <libthermometry.so>\n", argv[0]);
        return 2;
    }

    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("dlopen OK: %s\n", argv[1]);

    for (i = 0; names[i]; i++) {
        void *p = dlsym(h, names[i]);
        if (p)
            printf("  %-24s ok   (%p)\n", names[i], p);
        else {
            printf("  %-24s MISSING\n", names[i]);
            missing++;
        }
    }
    printf("resolved %d/10\n", 10 - missing);
    return missing ? 1 : 0;
}
