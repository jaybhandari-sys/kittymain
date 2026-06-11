/*
 * ap2p — edge_lite.h
 *
 * Tiny edge-server binding client.  Discovers our own server-reflexive (SRFLX)
 * IPv4 + port by sending a single 20-byte STUN-format Binding Request to the
 * configured edge_host:edge_port and parsing the XOR-MAPPED-ADDRESS reply.
 *
 * This is the v2.0 brand-neutral rename of src/provider-srt/stun_lite.h.
 * Functionality is byte-identical — only the public symbol names and macro
 * guard have been changed so that `strings` output on the final binary does
 * not leak "STUN" or the old product name into the read-only data segment.
 *
 * Usage:
 *   int fd = socket(AF_INET, SOCK_DGRAM, 0);
 *   bind(fd, ...);                                    // bind a local port
 *   ap2p_edge_binding_t b;
 *   ap2p_edge_get_srflx(fd, "edge.example.com", 3478, &b, 3);
 *   // b.srflx_ip / b.srflx_port now contain our public 5-tuple.
 *   // The caller can keep `fd` open for later libsrt re-bind to the same
 *   // local port so the NAT mapping persists across the handoff.
 */
#ifndef AP2P_EDGE_LITE_H
#define AP2P_EDGE_LITE_H

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
 * hairpin and therefore drop the rendezvous packets.
 *
 * Writes a printable IPv4 string into out (size cap).  Returns 0 on
 * success, -1 if no plausible address found. */
static inline int ap2p_edge_get_lan_ip(char *out, size_t cap) {
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
        if ((a & 0xffff0000u) == 0xa9fe0000u) continue;  /* 169.254/16 */
        if ((a & 0xff000000u) == 0x7f000000u) continue;  /* 127/8 */
        int is_rfc1918 = ((a & 0xff000000u) == 0x0a000000u)
                       || ((a & 0xfff00000u) == 0xac100000u)
                       || ((a & 0xffff0000u) == 0xc0a80000u);
        if (rc != 0 || is_rfc1918) {
            inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap);
            rc = 0;
            if (is_rfc1918) break;
        }
    }
    freeifaddrs(ifa);
    return rc;
}

typedef struct {
    char srflx_ip[64];
    int  srflx_port;
} ap2p_edge_binding_t;

#define AP2P_EDGE_MAGIC_COOKIE 0x2112A442u

static inline int _ap2p_edge_random_txn(uint8_t out[12]) {
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
static inline int ap2p_edge_get_srflx(int fd, const char *host, int port,
                                      ap2p_edge_binding_t *out, int retries) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai) return -1;

    /* 20-byte Binding Request:
     *   uint16 message_type   = 0x0001
     *   uint16 message_length = 0x0000  (no attributes)
     *   uint32 magic_cookie   = 0x2112A442
     *   uint8  txn_id[12]     = random
     */
    uint8_t req[20];
    req[0] = 0x00; req[1] = 0x01;
    req[2] = 0x00; req[3] = 0x00;
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xa4; req[7] = 0x42;
    _ap2p_edge_random_txn(req + 8);

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
        if (memcmp(buf + 8, req + 8, 12) != 0) continue;
        uint16_t mtype = (buf[0] << 8) | buf[1];
        if (mtype != 0x0101) continue;
        uint16_t mlen  = (buf[2] << 8) | buf[3];

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
                int xp = xport ^ (AP2P_EDGE_MAGIC_COOKIE >> 16);
                uint32_t xaddr = ((uint32_t)v[4] << 24) | ((uint32_t)v[5] << 16) |
                                 ((uint32_t)v[6] << 8)  | (uint32_t)v[7];
                uint32_t addr = xaddr ^ AP2P_EDGE_MAGIC_COOKIE;
                snprintf(out->srflx_ip, sizeof(out->srflx_ip), "%u.%u.%u.%u",
                         (addr >> 24) & 0xff, (addr >> 16) & 0xff,
                         (addr >> 8) & 0xff, addr & 0xff);
                out->srflx_port = xp;
                rc = 0; break;
            }
            if (atype == 0x0001 && alen >= 8 && v[1] == 0x01) {
                uint16_t mp = (v[2] << 8) | v[3];
                snprintf(out->srflx_ip, sizeof(out->srflx_ip), "%u.%u.%u.%u",
                         v[4], v[5], v[6], v[7]);
                out->srflx_port = mp;
                rc = 0;
            }
            size_t pad = (size_t)((4 - (alen & 3)) & 3);
            off += 4 + alen + pad;
        }
        if (rc == 0) break;
    }
    freeaddrinfo(ai);
    return rc;
}

#endif /* AP2P_EDGE_LITE_H */
