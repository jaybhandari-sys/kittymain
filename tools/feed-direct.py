#!/usr/bin/env python3
"""
feed-direct.py — fast-path viewer.  STUN → signaling → SRT rendezvous → patch
FLV header → write raw FLV bytes to stdout.

Designed to be the producer of a unix pipe:

    tools/feed-direct.py | ffplay -fflags nobuffer -flags low_delay -i pipe:0

What this drops vs `augentix_view.py`:

  - No 256-KB prefix replay cache (FlvFanout).  ffplay attaches to LIVE
    bytes — no 5-second stale-stream catch-up.
  - No local HTTP-FLV server, no jessibuca page, no threading fanout.
    Saves ~300 ms of Python startup + the entire HTTP socket round-trip.
  - No "wait for first FLV bytes" log-scrape in view-feed.sh — bytes go
    straight to ffplay's stdin the instant SRT recv returns them.

Total cold-start saving vs the legacy path: roughly 4–6 s.

What stays identical:

  - STUN → signaling → SRT-rendezvous sequence (camera-side expects this).
  - SRT options (SRTT_FILE stream-mode, latency 300 ms, rendezvous true).
  - FLV header DataOffset patch (camera emits DataOffset=0; FLV spec
    says 9; ffplay's demuxer rejects 0).

Usage:

    feed-direct.py [--latency-ms 300] [--service-id ATPL-...]
                   [--stun-host ...] [--signaling-host ...]
                   [--channel-path /flv/live_ch0_0.flv]

Env:

    ARCISAI_API_TOKEN    signaling registration token
    ARCISAI_VERIFY_TOKEN HTTP-FLV verify=... token (URL-encoded)

Exit:

    0 on clean EOF / SIGPIPE, 1 on any STUN/signaling/SRT failure.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import os
import secrets
import socket
import struct
import sys
import time


# ─────────────────────────── libsrt ctypes ──────────────────────────────────
_SRT_CANDIDATES = [
    "/opt/homebrew/lib/libsrt.dylib",
    "/usr/local/lib/libsrt.dylib",
    "/usr/lib/x86_64-linux-gnu/libsrt.so.1",
    "/usr/lib/libsrt.so.1",
]
_libsrt_path = next((p for p in _SRT_CANDIDATES if os.path.exists(p)), None)
if not _libsrt_path:
    p = ctypes.util.find_library("srt")
    if p:
        _libsrt_path = p
if not _libsrt_path:
    sys.exit("feed-direct: libsrt not found — `brew install srt`")
srt = ctypes.CDLL(_libsrt_path)

SRT_INVALID_SOCK = -1
SRT_ERROR        = -1

SRTO_RCVSYN        =  2
SRTO_RCVBUF        =  6
SRTO_RENDEZVOUS    = 12
SRTO_RCVTIMEO      = 14
SRTO_LATENCY       = 23
SRTO_CONNTIMEO     = 36
SRTO_RCVLATENCY    = 43
SRTO_PEERLATENCY   = 44
SRTO_MESSAGEAPI    = 48
SRTO_TRANSTYPE     = 50
SRTO_PEERIDLETIMEO = 55

SRTT_FILE = 1

srt.srt_startup.restype = ctypes.c_int
srt.srt_create_socket.restype = ctypes.c_int
srt.srt_setsockopt.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
srt.srt_setsockopt.restype  = ctypes.c_int

if sys.platform == "darwin":
    class sockaddr_in(ctypes.Structure):
        _fields_ = [("sin_len", ctypes.c_uint8), ("sin_family", ctypes.c_uint8),
                    ("sin_port", ctypes.c_uint16), ("sin_addr", ctypes.c_uint32),
                    ("sin_zero", ctypes.c_ubyte * 8)]
else:
    class sockaddr_in(ctypes.Structure):
        _fields_ = [("sin_family", ctypes.c_uint16), ("sin_port", ctypes.c_uint16),
                    ("sin_addr", ctypes.c_uint32), ("sin_zero", ctypes.c_ubyte * 8)]

srt.srt_bind.argtypes    = [ctypes.c_int, ctypes.POINTER(sockaddr_in), ctypes.c_int]
srt.srt_bind.restype     = ctypes.c_int
srt.srt_connect.argtypes = [ctypes.c_int, ctypes.POINTER(sockaddr_in), ctypes.c_int]
srt.srt_connect.restype  = ctypes.c_int
srt.srt_send.argtypes    = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
srt.srt_send.restype     = ctypes.c_int
srt.srt_recv.argtypes    = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
srt.srt_recv.restype     = ctypes.c_int
srt.srt_close.argtypes   = [ctypes.c_int]
srt.srt_close.restype    = ctypes.c_int
srt.srt_getlasterror_str.restype = ctypes.c_char_p


def _srterr() -> str:
    return srt.srt_getlasterror_str().decode("utf-8", "replace")


def _fill_sockaddr(addr: "sockaddr_in", host: str, port: int) -> None:
    if sys.platform == "darwin":
        addr.sin_len = ctypes.sizeof(sockaddr_in)
    addr.sin_family = socket.AF_INET
    addr.sin_port   = socket.htons(port)
    addr.sin_addr   = struct.unpack("!I",
                                    socket.inet_pton(socket.AF_INET, host))[0]
    addr.sin_addr   = socket.htonl(addr.sin_addr)
    addr.sin_zero   = (ctypes.c_ubyte * 8)()


# ────────────────────────────── STUN ─────────────────────────────────────────
def stun_discover(udp: socket.socket, host: str, port: int,
                  timeout: float = 2.0) -> tuple[str, int]:
    """Minimal STUN Binding Request.  Returns (srflx_ip, srflx_port)."""
    tx_id = secrets.token_bytes(12)
    req = struct.pack(">HHI", 0x0001, 0, 0x2112A442) + tx_id
    udp.sendto(req, (host, port))
    udp.settimeout(timeout)
    data, _ = udp.recvfrom(2048)
    if len(data) < 20 or data[8:20] != tx_id:
        raise RuntimeError("stun: bad response")
    # Skip the 20-byte header, parse XOR-MAPPED-ADDRESS (type 0x0020).
    i = 20
    while i + 4 <= len(data):
        atype = struct.unpack(">H", data[i:i + 2])[0]
        alen  = struct.unpack(">H", data[i + 2:i + 4])[0]
        body  = data[i + 4:i + 4 + alen]
        if atype == 0x0020 and len(body) >= 8:
            xport = struct.unpack(">H", body[2:4])[0] ^ 0x2112
            xip   = struct.unpack(">I", body[4:8])[0] ^ 0x2112A442
            return socket.inet_ntoa(struct.pack(">I", xip)), xport
        i += 4 + ((alen + 3) & ~3)
    raise RuntimeError("stun: no XOR-MAPPED-ADDRESS")


# ──────────────────────────── signaling ──────────────────────────────────────
def _write_frame(sock: socket.socket, payload: str) -> None:
    b = payload.encode()
    sock.sendall(struct.pack(">I", len(b)) + b)


def _read_frame(sock: socket.socket, timeout: float = 6.0) -> str:
    sock.settimeout(timeout)
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk: raise RuntimeError("sig: closed reading length")
        hdr += chunk
    n = struct.unpack(">I", hdr)[0]
    if n > 65536: raise RuntimeError(f"sig: absurd frame len {n}")
    body = b""
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        if not chunk: raise RuntimeError("sig: closed reading body")
        body += chunk
    return body.decode("utf-8", "replace")


def signaling_request(host: str, port: int, *,
                      service_id: str, api_token: str,
                      srflx_ip: str, srflx_port: int) -> tuple[str, int]:
    """Send SRT_REQUEST; return provider's (srflx_ip, srflx_port)."""
    payload = (
        f"type=SRT_REQUEST\n"
        f"service_id={service_id}\n"
        f"srflx_ip={srflx_ip}\n"
        f"srflx_port={srflx_port}\n"
        f"{'api_token=' + api_token + chr(10) if api_token else ''}"
    )
    s = socket.create_connection((host, port), timeout=5)
    try:
        _write_frame(s, payload)
        resp = _read_frame(s)
    finally:
        s.close()
    kv = dict(line.split("=", 1) for line in resp.splitlines() if "=" in line)
    if kv.get("type") != "SRT_PROVIDER":
        raise RuntimeError(f"sig: bad type {kv.get('type')!r} resp={resp!r}")
    if "srflx_ip" not in kv or "srflx_port" not in kv:
        raise RuntimeError(f"sig: missing srflx_*: {kv}")
    return kv["srflx_ip"], int(kv["srflx_port"])


# ─────────────────────────── SRT rendezvous ──────────────────────────────────
def srt_rendezvous(local_port: int, peer_ip: str, peer_port: int,
                   latency_ms: int = 300) -> int:
    if srt.srt_startup() < 0:
        raise RuntimeError("srt_startup")
    s = srt.srt_create_socket()
    if s == SRT_INVALID_SOCK:
        raise RuntimeError(f"srt_create_socket: {_srterr()}")

    def opt(o, val):
        v = ctypes.c_int(val)
        if srt.srt_setsockopt(s, 0, o, ctypes.byref(v),
                              ctypes.sizeof(v)) == SRT_ERROR:
            raise RuntimeError(f"setsockopt({o}): {_srterr()}")

    opt(SRTO_RENDEZVOUS, 1)
    opt(SRTO_TRANSTYPE,  SRTT_FILE)
    opt(SRTO_MESSAGEAPI, 0)
    opt(SRTO_LATENCY,    latency_ms)
    opt(SRTO_RCVLATENCY, latency_ms)
    opt(SRTO_PEERLATENCY, latency_ms)
    opt(SRTO_CONNTIMEO,  4000)
    opt(SRTO_PEERIDLETIMEO, 30000)
    opt(SRTO_RCVBUF, 4 * 1024 * 1024)

    la = sockaddr_in()
    _fill_sockaddr(la, "0.0.0.0", local_port)
    if srt.srt_bind(s, ctypes.byref(la), ctypes.sizeof(la)) == SRT_ERROR:
        srt.srt_close(s)
        raise RuntimeError(f"srt_bind :{local_port}: {_srterr()}")

    peer = sockaddr_in()
    _fill_sockaddr(peer, peer_ip, peer_port)
    if srt.srt_connect(s, ctypes.byref(peer), ctypes.sizeof(peer)) == SRT_ERROR:
        srt.srt_close(s)
        raise RuntimeError(f"srt_connect: {_srterr()}")
    return s


# ─────────────────────────────── main ────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stun-host",      default="turn.devices.arcisai.io")
    ap.add_argument("--stun-port",      default=5349, type=int)
    ap.add_argument("--signaling-host", default="signaling.devices.arcisai.io")
    ap.add_argument("--signaling-port", default=80, type=int)
    ap.add_argument("--api-token",      default=os.environ.get("ARCISAI_API_TOKEN", ""))
    ap.add_argument("--service-id",     default="ATPL-200007-TESTA")
    ap.add_argument("--verify-token",   default=os.environ.get("ARCISAI_VERIFY_TOKEN", ""))
    ap.add_argument("--channel-path",   default="/flv/live_ch0_0.flv")
    ap.add_argument("--latency-ms",     default=300, type=int)
    ap.add_argument("--quiet", "-q",    action="store_true",
                    help="suppress diagnostic prints to stderr")
    args = ap.parse_args()

    def log(msg: str) -> None:
        if not args.quiet:
            print(msg, file=sys.stderr, flush=True)

    t0 = time.monotonic()

    # 1. STUN binding (reuse the same UDP port for srt_bind).
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    local_port = udp.getsockname()[1]
    log(f"[time {time.monotonic()-t0:5.3f}s] stun → {args.stun_host}:{args.stun_port} (port {local_port})")
    try:
        srflx_ip, srflx_port = stun_discover(udp, args.stun_host, args.stun_port)
    except Exception as e:
        sys.exit(f"feed-direct: stun: {e}")
    finally:
        udp.close()
    log(f"[time {time.monotonic()-t0:5.3f}s] stun OK: SRFLX={srflx_ip}:{srflx_port}")

    # 2. Signaling.
    log(f"[time {time.monotonic()-t0:5.3f}s] signaling → {args.signaling_host}:{args.signaling_port}")
    try:
        peer_ip, peer_port = signaling_request(
            args.signaling_host, args.signaling_port,
            service_id=args.service_id, api_token=args.api_token,
            srflx_ip=srflx_ip, srflx_port=srflx_port,
        )
    except Exception as e:
        sys.exit(f"feed-direct: signaling: {e}")
    log(f"[time {time.monotonic()-t0:5.3f}s] signaling OK: provider {peer_ip}:{peer_port}")

    # 3. SRT rendezvous.
    log(f"[time {time.monotonic()-t0:5.3f}s] srt rendezvous {peer_ip}:{peer_port}")
    try:
        sock = srt_rendezvous(local_port, peer_ip, peer_port,
                              latency_ms=args.latency_ms)
    except Exception as e:
        sys.exit(f"feed-direct: srt: {e}")
    log(f"[time {time.monotonic()-t0:5.3f}s] srt UP")

    # 4. Send HTTP-FLV GET over SRT.
    query = f"?verify={args.verify_token}" if args.verify_token else ""
    http_req = (
        f"GET {args.channel_path}{query} HTTP/1.1\r\n"
        f"Host: 127.0.0.1\r\n"
        f"Authorization: Basic YWRtaW46YWRtaW4=\r\n"
        f"User-Agent: feed-direct/1.0\r\n"
        f"\r\n"
    ).encode("ascii")
    n = srt.srt_send(sock, http_req, len(http_req))
    if n < 0:
        sys.exit(f"feed-direct: srt_send GET: {_srterr()}")

    # 5. Recv loop.  IMPORTANT: the camera-side provider relays its local
    #    HTTP-FLV server's BODY only (no HTTP/1.1 response line / headers
    #    over SRT).  So the first bytes from srt_recv ARE the FLV magic
    #    "FLV\x01...".  We just patch DataOffset on those first 9 bytes
    #    (camera's libflv emits 0; the spec says 9) and forward everything.
    log(f"[time {time.monotonic()-t0:5.3f}s] piping FLV bytes to stdout...")

    buf = ctypes.create_string_buffer(65536)
    flv_header_patched = False
    saw_first_byte    = False
    total_bytes = 0
    out = sys.stdout.buffer

    try:
        while True:
            n = srt.srt_recv(sock, buf, len(buf))
            if n == SRT_ERROR:
                msg = _srterr()
                if "asyncrcv" in msg.lower() or "expired" in msg.lower():
                    continue
                log(f"feed-direct: srt_recv: {msg}")
                break
            if n == 0:
                # Stream-mode SRT can briefly return 0; only treat as EOF
                # after we've actually seen bytes.
                if saw_first_byte: break
                continue
            chunk = bytes(buf[:n])

            if not saw_first_byte:
                saw_first_byte = True
                log(f"[time {time.monotonic()-t0:5.3f}s] first SRT bytes "
                    f"({n} B, head={chunk[:9].hex()})")

            if (not flv_header_patched and total_bytes == 0
                    and len(chunk) >= 9 and chunk[:3] == b"FLV"):
                chunk = chunk[:5] + b"\x00\x00\x00\x09" + chunk[9:]
                flv_header_patched = True
                log(f"[time {time.monotonic()-t0:5.3f}s] FLV header patched "
                    f"(DataOffset → 9)")
            elif total_bytes == 0:
                # Don't keep checking for FLV magic; even if we never saw it,
                # let ffplay decide whether the bytes are decodable.
                flv_header_patched = True

            try:
                out.write(chunk)
                out.flush()
            except (BrokenPipeError, OSError):
                log("feed-direct: stdout closed (consumer gone)")
                break
            total_bytes += len(chunk)
    finally:
        srt.srt_close(sock)
        log(f"[time {time.monotonic()-t0:5.3f}s] streamed {total_bytes} bytes")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
