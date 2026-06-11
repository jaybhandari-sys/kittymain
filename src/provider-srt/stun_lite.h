/* Tiny STUN binding-request client.  Discovers our own SRFLX (server-reflexive)
 * IP and port by sending a single STUN Binding Request to a STUN/TURN server
 * and parsing the XOR-MAPPED-ADDRESS response.
 *
 * Used by provider_srt and the Android native module.  Designed to be
 * dependency-free and to share a UDP socket fd with later SRT use so the
 * NAT mapping persists.
 *
 * Usage:
 *   int fd = socket(AF_INET, SOCK_DGRAM, 0);
 *   bind(fd, ...);                      // bind to a chosen local port
 *   stun_binding_t out;
 *   stun_get_srflx(fd, "134.209.155.47", 3478, &out, 3);
 *   // out.srflx_ip / out.srflx_port now contain our public 5-tuple.
 *   // The caller can keep `fd` open for keepalives (re-call stun_get_srflx
 *   // periodically) until they're ready to hand the port off to SRT.
 */
#ifndef ARCIS_STUN_LITE_H
#define ARCIS_STUN_LITE_H

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>

/* IFF_UP / IFF_LOOPBACK live in <net/if.h>, but the Augentix uClibc sysroot
 * lays them out differently and including that header pulls in conflicting
 * decls.  These values are part of the stable Linux net ABI. */
#ifndef IFF_UP
#  define IFF_UP        0x1
#endif
#ifndef IFF_LOOPBACK
#  define IFF_LOOPBACK  0x8
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Find this host's primary LAN IPv4 — the first non-loopback, non-link-local
 * address.  Used so that two peers behind the same NAT (e.g. phone and
 * camera on the same office Wi-Fi) can rendezvous to each other's LAN
 * address instead of their shared public IP, which most NATs do not
 * hairpin and therefore drops the rendezvous packets.
 *
 * Writes a printable IPv4 string into out (size cap).  Returns 0 on
 * success, -1 if no plausible address found. */
static inline int stun_get_lan_ip(char *out, size_t cap) {
    if (!out || cap < 8) return -1;
    out[0] = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return -1;
    int rc = -1;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP)) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_in *sin = (struct sockaddr_in*)p->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        /* Skip 169.254.0.0/16 link-local — never a valid LAN candidate. */
        if ((a & 0xffff0000u) == 0xa9fe0000u) continue;
        /* Skip 127.0.0.0/8 just in case (already filtered by IFF_LOOPBACK). */
        if ((a & 0xff000000u) == 0x7f000000u) continue;
        /* Prefer RFC1918 ranges first — those are the typical LAN cases. */
        int is_rfc1918 = ((a & 0xff000000u) == 0x0a000000u)              /* 10/8 */
                       || ((a & 0xfff00000u) == 0xac100000u)             /* 172.16/12 */
                       || ((a & 0xffff0000u) == 0xc0a80000u);            /* 192.168/16 */
        if (rc != 0 || is_rfc1918) {
            inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap);
            rc = 0;
            if (is_rfc1918) break;  /* good enough, stop scanning */
        }
    }
    freeifaddrs(ifa);
    return rc;
}

typedef struct {
    char srflx_ip[64];
    int  srflx_port;
} stun_binding_t;

#define ARCIS_STUN_MAGIC_COOKIE 0x2112A442u

static inline int _stun_random_txn(uint8_t out[12]) {
    static int seeded = 0;
    if (!seeded) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        srand((unsigned)(ts.tv_nsec ^ (ts.tv_sec << 16) ^ (uintptr_t)&out));
        seeded = 1;
    }
    for (int i = 0; i < 12; i++) out[i] = (uint8_t)(rand() & 0xff);
    return 0;
}

/* Returns 0 on success, -1 on failure.  Uses up to retries+1 attempts.
 * Caller is responsible for the socket fd; this function sends and recvs
 * but does not close. */
static inline int stun_get_srflx(int fd, const char *stun_host, int stun_port,
                                 stun_binding_t *out, int retries) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", stun_port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(stun_host, portstr, &hints, &ai) != 0 || !ai) return -1;

    /* STUN Binding Request:
     *   uint16 message_type   = 0x0001
     *   uint16 message_length = 0x0000  (no attributes)
     *   uint32 magic_cookie   = 0x2112A442
     *   uint8  txn_id[12]     = random
     */
    uint8_t req[20];
    req[0] = 0x00; req[1] = 0x01;            /* type = Binding Request */
    req[2] = 0x00; req[3] = 0x00;            /* attributes length = 0 */
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xa4; req[7] = 0x42; /* magic */
    _stun_random_txn(req + 8);

    int rc = -1;
    for (int attempt = 0; attempt <= retries; attempt++) {
        if (sendto(fd, req, sizeof(req), 0, ai->ai_addr, ai->ai_addrlen) < 0) {
            continue;
        }
        struct timeval tv = { .tv_sec = 1 + attempt, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t buf[2048];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 20) continue;
        /* Match transaction id */
        if (memcmp(buf + 8, req + 8, 12) != 0) continue;
        /* Binding Success Response is exactly 0x0101 (method=0x001 in
         * msg-type bits 0-3 and 5-7 and 9-13; class success-response sets
         * bit 8 = 0x100).  Anything else is an error/wrong response.
         * (Earlier code used a bitmask that didn't actually isolate the
         * method bits — gcc warned that the `or` was always 1.) */
        uint16_t mtype = (buf[0] << 8) | buf[1];
        if (mtype != 0x0101) continue;
        uint16_t mlen  = (buf[2] << 8) | buf[3];

        /* Walk attributes looking for XOR-MAPPED-ADDRESS (0x0020) or
         * MAPPED-ADDRESS (0x0001) as fallback. */
        size_t off = 20;
        size_t end = 20 + (size_t)mlen;
        if (end > (size_t)n) end = (size_t)n;
        while (off + 4 <= end) {
            uint16_t atype = (buf[off]   << 8) | buf[off+1];
            uint16_t alen  = (buf[off+2] << 8) | buf[off+3];
            const uint8_t *v = buf + off + 4;
            if (off + 4 + alen > end) break;

            if (atype == 0x0020 && alen >= 8 && v[1] == 0x01) {
                uint16_t xport = (v[2] << 8) | v[3];
                int port = xport ^ (ARCIS_STUN_MAGIC_COOKIE >> 16);
                uint32_t xaddr = ((uint32_t)v[4] << 24) | ((uint32_t)v[5] << 16) |
                                 ((uint32_t)v[6] << 8)  | (uint32_t)v[7];
                uint32_t addr = xaddr ^ ARCIS_STUN_MAGIC_COOKIE;
                snprintf(out->srflx_ip, sizeof(out->srflx_ip), "%u.%u.%u.%u",
                         (addr >> 24) & 0xff, (addr >> 16) & 0xff,
                         (addr >> 8) & 0xff, addr & 0xff);
                out->srflx_port = port;
                rc = 0; break;
            }
            if (atype == 0x0001 && alen >= 8 && v[1] == 0x01) {
                uint16_t port = (v[2] << 8) | v[3];
                snprintf(out->srflx_ip, sizeof(out->srflx_ip), "%u.%u.%u.%u",
                         v[4], v[5], v[6], v[7]);
                out->srflx_port = port;
                rc = 0; /* keep scanning, prefer XOR if it shows up */
            }
            size_t pad = (size_t)((4 - (alen & 3)) & 3);
            off += 4 + alen + pad;
        }
        if (rc == 0) break;
    }
    freeaddrinfo(ai);
    return rc;
}

#endif
