/*
 * probe.c — Phase 0/1 bring-up tool for the DYT thermal camera.
 *
 * This is the first thing to run when the device arrives.  It uses
 * libusb directly (no libuvc), so it works before the capture layer is
 * built, and it is strictly READ-ONLY: it refuses to emit any opcode
 * whose table entry is marked WRITE, because the write path
 * (setMachineSetting / sendTinyCParamsModification /
 * setTinySaveCameraParams) can destroy factory calibration
 * irreversibly (RE Docs 04 §4.8).
 *
 *   probe                 enumerate USB devices, identify ours, show mode
 *   probe --descriptors   walk the UVC class descriptors and print every
 *                         streaming format (bFormatIndex / GUID / bpp /
 *                         frame sizes / fps).  This answers the one
 *                         outstanding frame-level unknown: which
 *                         bFormatIndex carries the thermal stream.
 *   probe --read          claim the interface and run the read-only
 *                         vendor transactions, printing the raw bytes
 *                         and the DecryptSNE-decoded serial.
 *   probe --op NAME       run one named opcode from the table (reads only)
 *
 * Options: --vid 0xXXXX --pid 0xXXXX --interface N
 *
 * build:  via the Makefile (make), which defines DYT_HAVE_LIBUSB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DYT_HAVE_LIBUSB
#error "probe requires libusb-1.0 (build with pkg-config libusb-1.0 available)"
#endif
#include <libusb.h>

#include "control.h"
#include "serial.h"

/* ------------------------------------------------------------------ devices */

/* Candidate VID/PID list (RE Docs 01 §1.3, 04 §4.10). */
static const struct {
    uint16_t vid, pid;
    const char *note;
} known[] = {
    { 0x1514, 0x0001, "DYT — full radiometric (mode 0x44c)" },
    { 0x0bda, 0x5840, "Realtek — direct AD (mode 1000)" },
    { 0x0bda, 0x5830, "Realtek — direct AD (mode 1000)" },
    { 0x0bda, 0x5846, "Realtek — variant (mode 0x3eb)" },
    { 0x0bda, 0x31da, "Realtek — variant (mode 0x3eb)" },
    { 0x0581, 0x0b00, "vendor — variant (mode 0x3eb)" },
};
#define KNOWN_N (int)(sizeof(known) / sizeof(known[0]))

static const char *mode_name(dyt_mode_t m)
{
    switch (m) {
      case DYT_MODE_44C:  return "0x44c radiometric";
      case DYT_MODE_1000: return "1000 direct-AD";
      case DYT_MODE_3EB:  return "0x3eb variant";
      default:            return "0 unsupported";
    }
}

static const char *known_note(uint16_t vid, uint16_t pid)
{
    int i;
    for (i = 0; i < KNOWN_N; i++)
        if (known[i].vid == vid && known[i].pid == pid)
            return known[i].note;
    return NULL;
}

/* ------------------------------------------------------------------ helpers */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* GUID -> the canonical Windows form.  A UVC GUID is stored with its first
 * three fields little-endian, so those bytes must be swapped back before
 * printing (UVC 1.1 §3.9.2 — the standard GUID wire format).  Getting this
 * wrong is easy to miss: the raw bytes still look plausible.  out must hold
 * at least 37 bytes. */
static void guid_str(const uint8_t g[16], char out[40])
{
    snprintf(out, 40,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0],          /* Data1, stored LE */
             g[5], g[4],                      /* Data2, stored LE */
             g[7], g[6],                      /* Data3, stored LE */
             g[8], g[9],                      /* Data4, stored BE */
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* Printable fourcc from the first 4 GUID bytes (e.g. "YUY2", "MJPG").
 * out must hold at least 5 bytes. */
static void fourcc_str(const uint8_t g[4], char out[5])
{
    int i;
    for (i = 0; i < 4; i++)
        out[i] = (g[i] >= 0x20 && g[i] < 0x7f) ? (char)g[i] : '.';
    out[4] = '\0';
}

/* ------------------------------------------------------------------ --list */

static int cmd_list(libusb_device **list, ssize_t n)
{
    ssize_t i;
    int matches = 0;

    printf("USB devices:\n");
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        uint8_t bus = libusb_get_bus_number(list[i]);
        uint8_t addr = libusb_get_device_address(list[i]);
        const char *note;

        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;

        note = known_note(d.idVendor, d.idProduct);
        printf("  %04x:%04x  bus %u addr %u", d.idVendor, d.idProduct, bus, addr);
        if (note) {
            dyt_mode_t m = dyt_mode_for_vidpid(d.idVendor, d.idProduct);
            printf("   <-- MATCH: %s  [%s]", note, mode_name(m));
            matches++;
        }
        printf("\n");
    }

    if (!matches) {
        printf("\nNo matching DYT/Realtek device present.\n");
        printf("Attach the camera and re-run; expect one of:\n");
        for (int i = 0; i < KNOWN_N; i++)
            printf("  %04x:%04x  %s\n", known[i].vid, known[i].pid, known[i].note);
    }
    return 0;
}

/* ----------------------------------------------------------- --descriptors */

static void print_frame_uncompressed(const uint8_t *p, int len)
{
    uint16_t w, h;
    uint32_t buf, interval;

    if (len < 26) return;
    w = le16(p + 5);
    h = le16(p + 7);
    buf = le32(p + 17);
    interval = le32(p + 21);

    printf("        frame[%u] %ux%u  buffer=%u B  default_interval=%u (%.2f fps)\n",
           p[3], w, h, buf, interval,
           interval ? 1.0e7 / (double)interval : 0.0);
}

static void print_frame_mjpeg(const uint8_t *p, int len)
{
    uint16_t w, h;
    uint32_t interval;

    if (len < 26) return;
    w = le16(p + 5);
    h = le16(p + 7);
    interval = le32(p + 21);

    printf("        frame[%u] %ux%u  default_interval=%u (%.2f fps)\n",
           p[3], w, h, interval,
           interval ? 1.0e7 / (double)interval : 0.0);
}

/* VideoControl (interface subclass 0x01) class-specific descriptors.
 *
 * These reuse the same subtype *numbers* as VideoStreaming but mean
 * completely different things — subtype 0x06 is EXTENSION_UNIT here and
 * VS_FORMAT_MJPEG there.  Decoding them with the VS meanings is what made
 * this tool report a bogus "FORMAT[4] MJPEG frames=140" on interface 0:
 * that descriptor is the EXTENSION_UNIT, and the 4 / 140 came from its
 * bUnitID and the first byte of its GUID.  (UVC 1.1 §3.7.) */
static void print_vc_descriptor(const uint8_t *p, int len, int subtype)
{
    switch (subtype) {
      case 0x01:   /* VC_HEADER */
        if (len < 12) break;
        printf("      VC header: bcdUVC=%u.%02u  %u interface(s)  clock=%.1f MHz\n",
               le16(p + 3) >> 8, le16(p + 3) & 0xff, p[11],
               le32(p + 7) / 1.0e6);
        break;

      case 0x02:   /* INPUT_TERMINAL */
        if (len < 8) break;
        printf("      INPUT_TERMINAL id=%u  type=0x%04x\n", p[3], le16(p + 4));
        break;

      case 0x03:   /* OUTPUT_TERMINAL */
        if (len < 8) break;
        printf("      OUTPUT_TERMINAL id=%u  type=0x%04x  source=%u\n",
               p[3], le16(p + 4), p[7]);
        break;

      case 0x04:   /* SELECTOR_UNIT */
        if (len < 6) break;
        printf("      SELECTOR_UNIT id=%u  %u input(s)\n", p[3], p[4]);
        break;

      case 0x05:   /* PROCESSING_UNIT */
        if (len < 9) break;
        printf("      PROCESSING_UNIT id=%u  source=%u  bControlSize=%u\n",
               p[3], p[4], p[7]);
        break;

      case 0x06: { /* EXTENSION_UNIT — byte order is bUnitID, GUID(16),
                    * bNumControls, bNrInPins, baSourceID, bControlSize,
                    * bmControls, iExtension */
        char g[40];
        if (len < 24) break;
        guid_str(p + 4, g);
        printf("      EXTENSION_UNIT id=%u  controls=%u  guid=%s\n",
               p[3], p[20], g);
        break;
      }

      default:
        break;
    }
}

/* VideoStreaming (interface subclass 0x02) class-specific descriptors. */
static void print_vs_descriptor(const uint8_t *p, int len, int subtype)
{
    switch (subtype) {
      case 0x01:   /* VS input header */
        if (len < 4) break;
        printf("      VS input header: %u format(s)\n", p[3]);
        break;

      case 0x04: { /* VS_FORMAT_UNCOMPRESSED */
        char fc[5], g[40];
        if (len < 27) break;
        fourcc_str(p + 5, fc);
        guid_str(p + 5, g);
        printf("      FORMAT[%u] UNCOMPRESSED  fourcc=%s  guid=%s  bpp=%u  frames=%u"
               "  default_frame=%u\n",
               p[3], fc, g, p[21], p[4], p[22]);
        break;
      }

      case 0x05:   /* VS_FRAME_UNCOMPRESSED */
        print_frame_uncompressed(p, len);
        break;

      case 0x06:   /* VS_FORMAT_MJPEG */
        if (len < 11) break;
        printf("      FORMAT[%u] MJPEG  frames=%u\n", p[3], p[4]);
        break;

      case 0x07:   /* VS_FRAME_MJPEG */
        print_frame_mjpeg(p, len);
        break;

      default:
        break;
    }
}

static void walk_uvc_extra(const uint8_t *extra, int extra_len, int subclass)
{
    const uint8_t *p = extra;
    int rem = extra_len;

    while (rem >= 3) {
        int len = p[0];
        int type = p[1];
        int subtype = p[2];

        if (len < 3 || len > rem)
            break;

        if (type == 0x24) {   /* class-specific (CS_INTERFACE) */
            if (subclass == 0x01)
                print_vc_descriptor(p, len, subtype);
            else if (subclass == 0x02)
                print_vs_descriptor(p, len, subtype);
        }
        p += len;
        rem -= len;
    }
}

static int cmd_descriptors(libusb_device *dev)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, a, e, rc;

    rc = libusb_get_config_descriptor(dev, 0, &cfg);
    if (rc != 0) {
        fprintf(stderr, "probe: get_config_descriptor: %s\n", libusb_strerror(rc));
        return 1;
    }

    printf("configuration %u: %u interface(s)\n",
           cfg->bConfigurationValue, cfg->bNumInterfaces);

    for (i = 0; i < cfg->bNumInterfaces; i++) {
        for (a = 0; a < cfg->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[i].altsetting[a];

            printf("  interface %u alt %u: class %02x subclass %02x proto %02x  (%u ep)\n",
                   id->bInterfaceNumber, id->bAlternateSetting,
                   id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol, id->bNumEndpoints);

            if (id->bInterfaceClass == 0x0e)   /* UVC */
                walk_uvc_extra(id->extra, id->extra_length, id->bInterfaceSubClass);

            for (e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                printf("      endpoint %02x: %s %s  max_packet=%u  interval=%u\n",
                       ep->bEndpointAddress,
                       (ep->bmAttributes & 0x03) == 0x01 ? "iso" :
                       (ep->bmAttributes & 0x03) == 0x02 ? "bulk" :
                       (ep->bmAttributes & 0x03) == 0x03 ? "int" : "ctrl",
                       (ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
                       ep->wMaxPacketSize, ep->bInterval);
                if (ep->extra_length)
                    walk_uvc_extra(ep->extra, ep->extra_length, 0);
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    return 0;
}

/* ------------------------------------------------------------------ --read */

static const dyt_opcode_t *find_op(const char *name)
{
    int i;
    for (i = 0; i < dyt_opcodes_n; i++)
        if (strcmp(dyt_opcodes[i].name, name) == 0)
            return &dyt_opcodes[i];
    return NULL;
}

static int is_write_op(const dyt_opcode_t *op)
{
    return op->note && strstr(op->note, "WRITE") != NULL;
}

static void hexdump(const char *label, const uint8_t *b, int n)
{
    int i;
    printf("  %-26s (%2d B) ", label, n);
    for (i = 0; i < n; i++) printf("%02x ", b[i]);
    printf("\n");
}

/* Run one named opcode.  Refuses WRITE entries outright. */
static int run_op(libusb_device_handle *h, const char *name)
{
    const dyt_opcode_t *op = find_op(name);
    uint8_t result[28];
    int status = 0, rc, len;

    if (!op) {
        fprintf(stderr, "probe: no such opcode '%s'\n", name);
        return 1;
    }
    if (is_write_op(op)) {
        fprintf(stderr, "probe: REFUSING '%s' — %s\n", name, op->note);
        fprintf(stderr, "       the write path can destroy factory calibration "
                        "(RE Docs 04 §4.8).\n");
        return 1;
    }

    len = (strcmp(name, "getTinyCUserSnCoefficient") == 0) ? 28 : 15;
    memset(result, 0, sizeof result);

    printf("  op '%s' -> wIndex 0x%04x, %d-byte result\n", name, op->wIndex, len);
    rc = dyt_transaction_ex(dyt_libusb_transfer(NULL), h,
                            op->cmd, op->wIndex, result, len, &status);
    if (rc != 0) {
        printf("    transaction failed (rc=%d, last status 0x%02x)\n",
               rc, (unsigned)status);
        return 1;
    }

    hexdump(name, result, len);

    if (strcmp(name, "getTinyCUserData") == 0) {
        uint8_t sn[15];
        char s[16];
        dyt_decrypt_sne(sn, result);
        dyt_serial_str(s, sn);
        printf("    DecryptSNE -> \"%s\"  (variant %s)\n",
               s, dyt_serial_variant(sn) ? "C" : "T/other");
    }
    return 0;
}

/* The read-only set, in the order the vendor brings the device up. */
static const char *read_set[] = {
    "getTinyCUserData",
    "getTinyCParams",
    "setMachineSetting_read",
};
#define READ_SET_N (int)(sizeof(read_set) / sizeof(read_set[0]))

/* Hardware-free safety check: classify every opcode and assert that the
 * read set contains nothing write-classified.  This is the only way to
 * exercise the WRITE guard without a device attached. */
static int cmd_selftest(void)
{
    int i, fails = 0;

    printf("opcode safety classification (RE Docs 04 §4.8):\n");
    for (i = 0; i < dyt_opcodes_n; i++) {
        const dyt_opcode_t *op = &dyt_opcodes[i];
        printf("  %-28s wIndex 0x%04x  %s\n", op->name, op->wIndex,
               is_write_op(op) ? "WRITE  (blocked)" : "read/stream (allowed)");
    }

    printf("\nread set contains no write opcodes: ");
    for (i = 0; i < READ_SET_N; i++) {
        const dyt_opcode_t *op = find_op(read_set[i]);
        if (!op) {
            printf("FAIL ('%s' not in table)\n", read_set[i]);
            fails++;
        } else if (is_write_op(op)) {
            printf("FAIL ('%s' is WRITE-classified)\n", read_set[i]);
            fails++;
        }
    }
    printf("%s\n", fails ? "" : "OK");
    return fails ? 1 : 0;
}

static int cmd_read(libusb_device_handle *h, int iface)
{
    int i, fails = 0;

    printf("claiming interface %d (auto-detaching kernel driver)...\n", iface);
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, iface) != 0) {
        /* Control transfers are often still permitted; report and go on. */
        fprintf(stderr, "probe: claim_interface %d failed — control transfers "
                        "may still work (try --interface N, or a udev rule)\n",
                iface);
    }

    printf("running read-only transactions (no writes are ever issued):\n");
    for (i = 0; i < READ_SET_N; i++)
        fails += run_op(h, read_set[i]);

    libusb_release_interface(h, iface);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ --uvc */

/* UVC class-specific GET requests against an extension unit (USB Device Class
 * Definition for Video Devices §6.4).  Strictly read-only: GET_LEN, GET_INFO,
 * GET_CUR only — there is no SET_CUR here.
 *
 *   bmRequestType 0xA1 (IN, class, interface)
 *   bRequest      0x85 GET_LEN | 0x86 GET_INFO | 0x81 GET_CUR
 *   wValue        selector << 8
 *   wIndex        (bUnitID << 8) | bInterfaceNumber
 *
 * Why this matters: the module is dual-vision class, and those commonly select
 * the active sensor through an extension-unit control.  On this unit the
 * EXTENSION_UNIT is bUnitID 4, bNumControls 2, with bmControls 0x00,0x06 — i.e.
 * selectors 10 and 11 — so the "which sensor" switch may live here rather than
 * in the TinyC orders (RE Docs 04 §4.2). */
static int cmd_uvc(libusb_device_handle *h, int unit, int iface)
{
    int sel;

    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, iface) != 0)
        fprintf(stderr, "probe: claim_interface %d failed — control transfers "
                        "may still work\n", iface);

    printf("UVC extension unit %d on interface %d — read-only queries:\n",
           unit, iface);
    printf("  %-4s %-5s %-5s %-12s %s\n", "sel", "len", "info", "caps", "cur");

    for (sel = 1; sel <= 16; sel++) {
        uint8_t lenbuf[2] = { 0, 0 };
        uint8_t info[1] = { 0 };
        uint8_t cur[64];
        int len, ri, rc2, j;

        len = libusb_control_transfer(h, 0xA1, 0x85, (uint16_t)(sel << 8),
                                      (uint16_t)((unit << 8) | iface),
                                      lenbuf, 2, 1000);
        if (len != 2)
            continue;                    /* selector absent (typically STALLs) */
        len = lenbuf[0] | (lenbuf[1] << 8);

        ri = libusb_control_transfer(h, 0xA1, 0x86, (uint16_t)(sel << 8),
                                     (uint16_t)((unit << 8) | iface),
                                     info, 1, 1000);

        memset(cur, 0, sizeof cur);
        if (len > (int)sizeof cur)
            len = (int)sizeof cur;
        rc2 = libusb_control_transfer(h, 0xA1, 0x81, (uint16_t)(sel << 8),
                                      (uint16_t)((unit << 8) | iface),
                                      cur, (uint16_t)len, 1000);

        printf("  %-4d %-5d 0x%02x  ", sel, len, ri == 1 ? info[0] : 0);
        if (ri == 1) {
            /* GET_INFO bit0 GET, bit1 SET, bit2 DISABLED (UVC §6.4.3). */
            printf("%s%s%s ", (info[0] & 0x01) ? "GET" : "---",
                   (info[0] & 0x02) ? "SET" : "---",
                   (info[0] & 0x04) ? " DISABLED" : "");
        } else {
            printf("(GET_INFO rc=%d) ", ri);
        }
        if (rc2 == len) {
            for (j = 0; j < len; j++)
                printf("%02x ", cur[j]);
        } else {
            printf("(GET_CUR rc=%d)", rc2);
        }
        printf("\n");
    }

    libusb_release_interface(h, iface);
    return 0;
}

/* -------------------------------------------------------------------- main */

static uint16_t parse_u16(const char *s)
{
    return (uint16_t)strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    enum { M_LIST, M_DESC, M_READ, M_OP, M_SELFTEST, M_UVC } mode = M_LIST;
    const char *op_name = NULL;
    uint16_t want_vid = 0, want_pid = 0;
    int iface = 0, unit = 4, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--descriptors")) mode = M_DESC;
        else if (!strcmp(argv[i], "--read"))   mode = M_READ;
        else if (!strcmp(argv[i], "--list"))   mode = M_LIST;
        else if (!strcmp(argv[i], "--selftest")) mode = M_SELFTEST;
        else if (!strcmp(argv[i], "--uvc"))    mode = M_UVC;
        else if (!strcmp(argv[i], "--unit") && i + 1 < argc)
            unit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--op") && i + 1 < argc) {
            mode = M_OP;
            op_name = argv[++i];
        }
        else if (!strcmp(argv[i], "--vid") && i + 1 < argc)
            want_vid = parse_u16(argv[++i]);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc)
            want_pid = parse_u16(argv[++i]);
        else if (!strcmp(argv[i], "--interface") && i + 1 < argc)
            iface = atoi(argv[++i]);
        else {
            fprintf(stderr,
                "usage: %s [--list|--descriptors|--read|--uvc|--op NAME|--selftest] "
                "[--vid 0xXXXX] [--pid 0xXXXX] [--interface N] [--unit N]\n", argv[0]);
            return 2;
        }
    }

    /* --selftest needs neither libusb nor a device. */
    if (mode == M_SELFTEST)
        return cmd_selftest();

    libusb_device **list = NULL;
    libusb_context *ctx = NULL;
    ssize_t n;
    int rc, ret = 0;

    rc = libusb_init(&ctx);
    if (rc != 0) {
        fprintf(stderr, "probe: libusb_init: %s\n", libusb_strerror(rc));
        return 1;
    }

    n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        fprintf(stderr, "probe: get_device_list failed\n");
        libusb_exit(ctx);
        return 1;
    }

    if (mode == M_LIST) {
        ret = cmd_list(list, n);
        goto done;
    }

    /* Locate the target device: explicit VID/PID, else first known match. */
    libusb_device *dev = NULL;
    for (ssize_t k = 0; k < n; k++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[k], &d) != 0)
            continue;
        if (want_vid && want_pid) {
            if (d.idVendor == want_vid && d.idProduct == want_pid) { dev = list[k]; break; }
        } else if (known_note(d.idVendor, d.idProduct)) {
            dev = list[k];
            want_vid = d.idVendor;
            want_pid = d.idProduct;
            break;
        }
    }

    if (!dev) {
        fprintf(stderr, "probe: no matching device found (run '%s' to list)\n",
                argv[0]);
        ret = 1;
        goto done;
    }

    printf("device %04x:%04x -> mode %s\n", want_vid, want_pid,
           mode_name(dyt_mode_for_vidpid(want_vid, want_pid)));

    if (mode == M_DESC) {
        ret = cmd_descriptors(dev);
        goto done;
    }

    /* --read / --op need the device open. */
    libusb_device_handle *h = NULL;
    rc = libusb_open(dev, &h);
    if (rc != 0) {
        fprintf(stderr, "probe: open: %s (check permissions / udev rule)\n",
                libusb_strerror(rc));
        ret = 1;
        goto done;
    }

    if (mode == M_UVC) {
        ret = cmd_uvc(h, unit, iface);
    } else if (mode == M_OP) {
        ret = run_op(h, op_name);
    } else {
        ret = cmd_read(h, iface);
    }

    libusb_close(h);

done:
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return ret;
}
