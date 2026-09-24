/* isotest — raw isochronous probe for the DYT/Mechanic iScout thermal camera.
 *
 * Why this exists: libuvc chooses the streaming altsetting itself and ignores
 * uvc_stream_ctrl_t.bAlternateSetting (third_party/libuvc/src/stream.c: it
 * walks interface->altsetting[] and takes the first whose wMaxPacketSize is
 * >= the negotiated dwMaxPayloadTransferSize).  On this unit that path yields
 * a flat 0x8000 frame at roughly one frame per 40 s, while the Windows vendor
 * app gets real 16-bit data at ~31 C.  The vendor's isochronous packets are
 * 524 B (12 B UVC header + 512 B payload = one 256-pixel row), which needs
 * wMaxPacketSize >= 524, i.e. altsetting 3 or higher.
 *
 * So this tool bypasses libuvc entirely: it claims interface 1, selects each
 * altsetting in turn, opens a raw isochronous stream on the VS endpoint, and
 * reports how many bytes actually arrive plus what the 16-bit values look
 * like.  That isolates "wrong altsetting / no bandwidth" from "device is not
 * producing thermal data".
 *
 * It is read-only.  It never issues a vendor control transfer: the only
 * requests are SET_INTERFACE (standard) and isochronous IN reads.
 *
 * usage: isotest [--vid 0xXXXX] [--pid 0xXXXX] [--iface N] [--ep 0xNN]
 *                [--seconds S] [--alt N] [--verbose]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#define DEF_VID    0x0bda
#define DEF_PID    0x5840
#define DEF_IFACE  1
#define DEF_EP     0x81
#define NUM_XFERS  8
#define PKTS_PER   8

/* Value histogram over the whole run, per altsetting.  A 16-bit space is only
 * 256 KiB, so a flat array is cheaper than any hash. */
static uint32_t hist[65536];
static unsigned long long total_bytes, total_pkts, data_pkts;
static unsigned char first_bytes[32];
static int first_bytes_n;
static int verbose;
static int stopping, returned;

static void note_payload(const unsigned char *p, int len)
{
    int i, hdr = 0;

    /* Strip the UVC payload header.  bHeaderLength is byte 0 and is one of
     * 2/6/12/14; byte 1 carries the FID bit (0x80) once streaming is live.
     * If it does not look like a header, treat the whole packet as payload —
     * we would rather histogram noise than silently drop real data. */
    if (len >= 2 && (p[0] == 2 || p[0] == 6 || p[0] == 12 || p[0] == 14) &&
        (p[1] & 0x80))
        hdr = p[0];
    if (hdr > len)
        hdr = 0;

    p += hdr;
    len -= hdr;

    if (first_bytes_n < (int)sizeof first_bytes) {
        int n = (int)sizeof first_bytes - first_bytes_n;
        if (n > len) n = len;
        memcpy(first_bytes + first_bytes_n, p, n);
        first_bytes_n += n;
    }

    for (i = 0; i + 1 < len; i += 2)
        hist[p[i] | (p[i + 1] << 8)]++;
}

static void LIBUSB_CALL iso_cb(struct libusb_transfer *t)
{
    int i;

    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
            if (d->status != LIBUSB_TRANSFER_COMPLETED || d->actual_length == 0)
                continue;
            total_pkts++;
            data_pkts++;
            total_bytes += d->actual_length;
            note_payload(libusb_get_iso_packet_buffer_simple(t, i),
                         (int)d->actual_length);
        }
    } else {
        total_pkts++;
    }

    if (stopping) {
        /* The transfer must not be freed until it has come back to us, so
         * do not resubmit and let test_alt() wait for the count to settle. */
        returned++;
        return;
    }

    if (libusb_submit_transfer(t) != 0)
        fprintf(stderr, "isotest: resubmit failed\n");
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Find EP `ep` in altsetting `alt` of `iface` and return its wMaxPacketSize,
 * already multiplied out by the high-bandwidth packet multiplier. */
static int ep_packet_size(libusb_device *dev, int iface, int alt, int ep)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, size = 0;

    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return -1;
    if (iface < 0 || iface >= cfg->bNumInterfaces) {
        libusb_free_config_descriptor(cfg);
        return -1;
    }

    for (i = 0; i < cfg->interface[iface].num_altsetting; i++) {
        const struct libusb_interface_descriptor *a =
            &cfg->interface[iface].altsetting[i];
        int e;
        if (a->bAlternateSetting != alt)
            continue;
        for (e = 0; e < a->bNumEndpoints; e++) {
            if (a->endpoint[e].bEndpointAddress == ep) {
                size = a->endpoint[e].wMaxPacketSize;
                size = (size & 0x07ff) * (((size >> 11) & 3) + 1);
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    return size ? size : -1;
}

static int test_alt(libusb_device_handle *h, libusb_device *dev, int iface,
                    int alt, int ep, double seconds)
{
    struct libusb_transfer *xfers[NUM_XFERS] = { 0 };
    unsigned char *bufs[NUM_XFERS] = { 0 };
    int pkt_size = ep_packet_size(dev, iface, alt, ep);
    int i, rc, nonzero = 0, distinct = 0;
    double t0, t1;
    uint16_t top[5] = { 0 };
    uint32_t topn[5] = { 0 };

    if (pkt_size < 0) {
        printf("alt %d: no endpoint 0x%02x\n", alt, ep);
        return -1;
    }

    rc = libusb_set_interface_alt_setting(h, iface, alt);
    if (rc != 0) {
        printf("alt %d: SET_INTERFACE failed (%s)  packet=%d\n",
               alt, libusb_error_name(rc), pkt_size);
        return -1;
    }

    memset(hist, 0, sizeof hist);
    total_bytes = total_pkts = data_pkts = 0;
    first_bytes_n = 0;

    stopping = 0;
    returned = 0;
    for (i = 0; i < NUM_XFERS; i++) {
        xfers[i] = libusb_alloc_transfer(PKTS_PER);
        bufs[i] = malloc((size_t)pkt_size * PKTS_PER);
        if (!xfers[i] || !bufs[i]) {
            fprintf(stderr, "isotest: out of memory\n");
            return -1;
        }
        libusb_fill_iso_transfer(xfers[i], h, (unsigned char)ep, bufs[i],
                                 pkt_size * PKTS_PER, PKTS_PER, iso_cb, NULL,
                                 1000);
        libusb_set_iso_packet_lengths(xfers[i], (unsigned int)pkt_size);
        if (libusb_submit_transfer(xfers[i]) != 0)
            fprintf(stderr, "isotest: submit failed\n");
    }

    t0 = now_s();
    while ((t1 = now_s()) - t0 < seconds) {
        struct timeval tv;
        double left = seconds - (t1 - t0);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (left < 0.2)
            tv.tv_usec = (long)(left * 1e6);
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    }

    /* Drain: stop resubmitting, then wait for every transfer to be handed
     * back before freeing it.  Freeing a still-submitted transfer is what
     * made an earlier version of this tool segfault. */
    stopping = 1;
    t0 = now_s();
    while (returned < NUM_XFERS && now_s() - t0 < 5.0) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    }

    for (i = 0; i < NUM_XFERS; i++) {
        libusb_free_transfer(xfers[i]);
        free(bufs[i]);
    }
    libusb_set_interface_alt_setting(h, iface, 0);

    for (i = 0; i < 65536; i++) {
        if (!hist[i])
            continue;
        distinct++;
        if (hist[i] > topn[0]) {
            topn[4] = topn[3]; top[4] = top[3];
            topn[3] = topn[2]; top[3] = top[2];
            topn[2] = topn[1]; top[2] = top[1];
            topn[1] = topn[0]; top[1] = top[0];
            topn[0] = hist[i]; top[0] = (uint16_t)i;
        }
    }
    if (top[0] != 0x8000)
        nonzero = 1;

    printf("alt %d: packet=%4d  bytes=%9llu  pkts=%6llu  distinct16=%5d  "
           "top=", alt, pkt_size, total_bytes, data_pkts, distinct);
    for (i = 0; i < 5; i++)
        if (topn[i])
            printf("0x%04x(%lu) ", top[i], (unsigned long)topn[i]);
    if (first_bytes_n >= 16)
        printf(" first=%02x%02x %02x%02x %02x%02x %02x%02x",
               first_bytes[0], first_bytes[1], first_bytes[2], first_bytes[3],
               first_bytes[4], first_bytes[5], first_bytes[6], first_bytes[7]);
    printf("%s\n", (distinct > 1 && nonzero) ? "   <== VARIES" : "");

    if (verbose && first_bytes_n) {
        printf("        first %d bytes:", first_bytes_n);
        for (i = 0; i < first_bytes_n; i++)
            printf(" %02x", first_bytes[i]);
        printf("\n");
    }

    return 0;
}

int main(int argc, char **argv)
{
    uint16_t vid = DEF_VID, pid = DEF_PID;
    int iface = DEF_IFACE, ep = DEF_EP, only_alt = -1, i;
    double seconds = 2.0;
    libusb_device **list = NULL;
    libusb_device_handle *h = NULL;
    libusb_device *dev = NULL;
    ssize_t n;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vid") && i + 1 < argc)
            vid = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc)
            pid = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)
            iface = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ep") && i + 1 < argc)
            ep = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--alt") && i + 1 < argc)
            only_alt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else {
            fprintf(stderr,
                "usage: %s [--vid 0xXXXX] [--pid 0xXXXX] [--iface N] "
                "[--ep 0xNN] [--seconds S] [--alt N] [--verbose]\n", argv[0]);
            return 2;
        }
    }

    if (libusb_init(NULL) != 0) {
        fprintf(stderr, "isotest: libusb_init failed\n");
        return 1;
    }

    n = libusb_get_device_list(NULL, &list);
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (d.idVendor == vid && d.idProduct == pid) {
            dev = list[i];
            break;
        }
    }
    if (!dev) {
        fprintf(stderr, "isotest: %04x:%04x not found\n", vid, pid);
        return 1;
    }

    if (libusb_open(dev, &h) != 0) {
        fprintf(stderr, "isotest: open failed (permissions?)\n");
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, iface) != 0) {
        fprintf(stderr, "isotest: claim interface %d failed\n", iface);
        return 1;
    }

    printf("isotest: %04x:%04x interface %d endpoint 0x%02x, %.1fs per "
           "altsetting\n\n", vid, pid, iface, ep, seconds);

    if (only_alt > 0) {
        test_alt(h, dev, iface, only_alt, ep, seconds);
    } else {
        for (i = 1; i <= 7; i++)
            test_alt(h, dev, iface, i, ep, seconds);
    }

    libusb_release_interface(h, iface);
    libusb_close(h);
    libusb_free_device_list(list, 1);
    libusb_exit(NULL);
    return 0;
}
