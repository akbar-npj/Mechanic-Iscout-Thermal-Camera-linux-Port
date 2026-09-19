/*
 * frame_test.c — unit tests for dyt_frame_resolve (frame.c).
 *
 * dyt_frame_resolve is the one piece of capture-layer logic that is
 * verifiable without hardware: it maps a received UVC payload size to
 * the frame_t geometry.  The risk it guards against is real — the UVC
 * stream descriptor may advertise either the active height (192) or the
 * full height including the reference band (196), and RE Docs 04 §4.5.1
 * does not settle which.  Deriving the row count from the byte count
 * makes the port work either way.
 *
 * build:  via the Makefile (make frame-test)
 */
#include <stdio.h>
#include <string.h>

#include "frame.h"

static int fails;

static void check(const char *what, dyt_mode_t mode, int width,
                  size_t data_bytes, int want_rc,
                  int want_active, int want_total, int want_rec_base)
{
    int active = -1, total = -1, rec_base = -1;
    int rc = dyt_frame_resolve(mode, width, data_bytes,
                               &active, &total, &rec_base);

    if (rc != want_rc) {
        printf("  FAIL %-46s rc=%d want %d\n", what, rc, want_rc);
        fails++;
        return;
    }
    if (rc != 0) {
        printf("  ok   %-46s rc=-1 (rejected)\n", what);
        return;
    }
    if (active != want_active || total != want_total ||
        rec_base != want_rec_base) {
        printf("  FAIL %-46s got %d/%d/0x%x want %d/%d/0x%x\n", what,
               active, total, rec_base,
               want_active, want_total, want_rec_base);
        fails++;
        return;
    }
    printf("  ok   %-46s active=%d total=%d rec_base=0x%x\n",
           what, active, total, rec_base);
}

int main(void)
{
    printf("=== frame_test (dyt_frame_resolve) ===\n");

    /* Mode 0x44c: payload carries active + REF_ROWS rows.  Sizes taken
     * from the frozen harness cases under tools/thermometry_diff/out. */
    check("0x44c 256x192 (+4 ref)", DYT_MODE_44C, 256,
          256u * 196u * 2u, 0, 192, 196, 0x200);
    check("0x44c 240x180 (+4 ref)", DYT_MODE_44C, 240,
          240u * 184u * 2u, 0, 180, 184, 0x1e0);
    check("0x44c 384x288 (+4 ref)", DYT_MODE_44C, 384,
          384u * 292u * 2u, 0, 288, 292, 0x900);
    check("0x44c 640x476 (+4 ref)", DYT_MODE_44C, 640,
          640u * 480u * 2u, 0, 476, 480, 0xf00);

    /* The ambiguity this function exists to absorb: a descriptor that
     * advertises only the active height produces a payload without the
     * reference band, which must be rejected rather than misparsed. */
    check("0x44c 256x192 (no ref band)", DYT_MODE_44C, 256,
          256u * 192u * 2u, -1, 0, 0, 0);

    /* Unknown sensor width. */
    check("0x44c unknown width 320", DYT_MODE_44C, 320,
          320u * 244u * 2u, -1, 0, 0, 0);

    /* Malformed payloads. */
    check("0x44c odd byte count", DYT_MODE_44C, 256,
          256u * 196u * 2u - 1u, -1, 0, 0, 0);
    check("0x44c empty payload", DYT_MODE_44C, 256, 0, -1, 0, 0, 0);
    check("0x44c zero width", DYT_MODE_44C, 0, 1024, -1, 0, 0, 0);

    /* Mode 1000: no reference band, so active == total and no record. */
    check("1000 256x192 (no ref band)", DYT_MODE_1000, 256,
          256u * 192u * 2u, 0, 192, 192, 0);
    check("1000 640x480", DYT_MODE_1000, 640,
          640u * 480u * 2u, 0, 480, 480, 0);

    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
