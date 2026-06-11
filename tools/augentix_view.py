#!/usr/bin/env python3
# Augentix P2P → local HTTP-FLV proxy + jessibuca page server.
#
# What it does (single process, one terminal):
#   1. UDP STUN bind against turn.devices.arcisai.io:5349 → learn our SRFLX.
#   2. TCP signaling (signaling.devices.arcisai.io:80, plain TCP) — see
#      ops/dev-integration-guide.md.  Send SRT_REQUEST with our SRFLX,
#      receive SRT_PROVIDER with the camera's SRFLX.
#   3. libsrt rendezvous to the camera's SRFLX on the SAME local UDP port the
#      STUN binding used (so the carrier NAT mapping survives the hand-off).
#      Stream mode (SRTT_FILE), 300 ms latency.
#   4. Send an HTTP-FLV GET over the SRT tunnel.
#   5. Spawn an HTTP server on 127.0.0.1:8080 that:
#        - serves /  → redirects to /player.html (the jessibuca bundle we
#          copied into the app's assets).
#        - serves /jessibuca/* from the asset dir (player.html loads
#          decoder-pro-*.js + .wasm relatively).
#        - serves /live.flv by piping bytes out of the SRT socket.
#
# Open http://127.0.0.1:8080/ in Chrome and the feed renders via jessibuca
# (same origin as the FLV → no CORS preflight, no mixed-content gotchas).
#
# Requires: brew install srt ffmpeg  (libsrt 1.5.x; ffmpeg only for fallback
# ffplay testing, optional).

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import http.server
import json
import os
import pathlib
import secrets
import socket
import socketserver
import struct
import sys
import threading
import time


# ============================================================================
# v2.0.6 per-session stream timing (viewer side).
#
# Pairs with the camera-side stream_timing module that publishes to
# torque/tx/<sn>/stream on first FLV-byte-pushed.  Together we get the full
# end-to-end picture of where the ~2 s connect-to-glass latency goes.
# ============================================================================
_TIMINGS: dict[str, float] = {}


def stamp(label: str) -> None:
    _TIMINGS[label] = time.monotonic()
    t0 = _TIMINGS.get("t_start", _TIMINGS[label])
    print(f"[time] +{_TIMINGS[label] - t0:6.3f}s  {label}", flush=True)


def dump_timings(path: str = "/tmp/viewer_session.json") -> None:
    t0 = _TIMINGS.get("t_start", 0.0)
    try:
        pathlib.Path(path).write_text(json.dumps({
            "version": "2.0.6",
            "absolute_monotonic_sec": dict(_TIMINGS),
            "since_t_start_sec":     {k: v - t0 for k, v in _TIMINGS.items()},
            "wall_clock_t_start":    time.time() - (time.monotonic() - t0),
        }, indent=2))
        print(f"[time] dumped → {path}", flush=True)
    except Exception as e:
        print(f"[time] dump failed: {e}", flush=True)
from dataclasses import dataclass

# ── libsrt ctypes binding ────────────────────────────────────────────────────
# We bind only what we need.  All ints/long shapes match libsrt 1.5.x on macOS.

_SRT_CANDIDATES = [
    "/opt/homebrew/lib/libsrt.dylib",
    "/opt/homebrew/opt/srt/lib/libsrt.dylib",
    "/usr/local/lib/libsrt.dylib",
    "/usr/lib/libsrt.dylib",
]
_libsrt_path = next((p for p in _SRT_CANDIDATES if os.path.exists(p)), None)
if not _libsrt_path:
    p = ctypes.util.find_library("srt")
    if p:
        _libsrt_path = p
if not _libsrt_path:
    sys.exit("libsrt not found — run: brew install srt")
srt = ctypes.CDLL(_libsrt_path)

SRT_INVALID_SOCK = -1
SRT_ERROR        = -1

# enum SRT_SOCKOPT — values from libsrt 1.5.x /opt/homebrew/include/srt/srt.h .
# Earlier libsrt revisions used different numbers; these are the current ones
# and must be kept in sync with the brew-installed dylib.
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
SRTO_PEERIDLETIMEO = 55   # peer-idle timeout in milliseconds (default 5s)

# SRTT_FILE = 1 (stream API) — provider_srt on the camera uses this; if we
# default to SRTT_LIVE the handshake gets rejected with "Agent uses MESSAGE
# API, but Peer declares STREAM API".
SRTT_FILE  = 1

srt.srt_startup.restype = ctypes.c_int
srt.srt_create_socket.restype = ctypes.c_int
srt.srt_setsockopt.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
srt.srt_setsockopt.restype  = ctypes.c_int

# macOS / BSD lay out sockaddr_in with a sin_len byte first and sa_family_t
# only one byte wide.  Linux lays it out with a 2-byte sin_family at the
# start and no sin_len.  We use sys.platform to pick.
if sys.platform == "darwin":
    class sockaddr_in(ctypes.Structure):
        _fields_ = [
            ("sin_len",    ctypes.c_uint8),
            ("sin_family", ctypes.c_uint8),
            ("sin_port",   ctypes.c_uint16),
            ("sin_addr",   ctypes.c_uint32),
            ("sin_zero",   ctypes.c_ubyte * 8),
        ]
else:
    class sockaddr_in(ctypes.Structure):
        _fields_ = [
            ("sin_family", ctypes.c_uint16),
            ("sin_port",   ctypes.c_uint16),
            ("sin_addr",   ctypes.c_uint32),
            ("sin_zero",   ctypes.c_ubyte * 8),
        ]

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


# ── STUN binding ─────────────────────────────────────────────────────────────

def stun_discover(local_udp: socket.socket, stun_host: str, stun_port: int,
                  timeout: float = 4.0) -> tuple[str, int]:
    """One classic STUN Binding Request; parse XOR-MAPPED-ADDRESS."""
    txn = secrets.token_bytes(12)
    # Type=0x0001 (Binding Request), Length=0, magic cookie, txn
    req = struct.pack("!HHI", 0x0001, 0, 0x2112A442) + txn
    local_udp.settimeout(timeout)
    local_udp.sendto(req, (stun_host, stun_port))
    data, _ = local_udp.recvfrom(2048)
    if len(data) < 20:
        raise RuntimeError(f"STUN: short reply ({len(data)}B)")
    msg_type = struct.unpack("!H", data[:2])[0]
    if msg_type != 0x0101:                              # Binding Success
        raise RuntimeError(f"STUN: msgtype=0x{msg_type:04x} (expected 0x0101)")
    i = 20
    while i + 4 <= len(data):
        at, al = struct.unpack("!HH", data[i:i+4])
        v = data[i+4 : i+4+al]
        if at == 0x0020 and len(v) >= 8:                # XOR-MAPPED-ADDRESS
            port = struct.unpack("!H", v[2:4])[0] ^ 0x2112
            ip_bytes = bytes(b ^ m for b, m in zip(v[4:8], b"\x21\x12\xa4\x42"))
            return socket.inet_ntoa(ip_bytes), port
        i += 4 + al + ((-al) % 4)
    raise RuntimeError("STUN: no XOR-MAPPED-ADDRESS attribute")


# ── Signaling protocol (TCP, 4-byte BE length prefix + INI text) ────────────

@dataclass
class ProviderInfo:
    service_id: str
    srflx_ip:   str
    srflx_port: int
    lan_ip:     str | None
    lan_port:   int | None
    raw:        dict[str, str]


def _write_frame(sock: socket.socket, payload: str) -> None:
    body = payload.encode("utf-8")
    sock.sendall(struct.pack("!I", len(body)) + body)


def _read_frame(sock: socket.socket) -> str:
    hdr = b""
    while len(hdr) < 4:
        chunk = sock.recv(4 - len(hdr))
        if not chunk:
            raise RuntimeError("signaling: server closed during header")
        hdr += chunk
    n = struct.unpack("!I", hdr)[0]
    if n == 0 or n > 65_536:
        raise RuntimeError(f"signaling: bad frame length {n}")
    body = b""
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        if not chunk:
            raise RuntimeError("signaling: server closed during body")
        body += chunk
    return body.decode("utf-8")


def signaling_request(host: str, port: int, *,
                      service_id: str, api_token: str,
                      srflx_ip: str, srflx_port: int,
                      timeout: float = 8.0) -> ProviderInfo:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        peer_id = f"py-{secrets.token_hex(4)}"
        payload = (
            f"type=SRT_REQUEST\n"
            f"api_token={api_token}\n"
            f"service_id={service_id}\n"
            f"srflx_ip={srflx_ip}\n"
            f"srflx_port={srflx_port}\n"
            f"peer_id={peer_id}\n"
        )
        _write_frame(sock, payload)
        reply = _read_frame(sock)
    finally:
        sock.close()

    kv: dict[str, str] = {}
    for line in reply.splitlines():
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        eq = line.find("=")
        if eq > 0:
            kv[line[:eq]] = line[eq + 1:]

    if kv.get("type") == "ERROR":
        raise RuntimeError(f"signaling ERROR: {kv.get('message', '(none)')}")
    if kv.get("type") != "SRT_PROVIDER":
        raise RuntimeError(f"signaling unexpected reply: {kv}")
    if not kv.get("srflx_ip") or not kv.get("srflx_port"):
        raise RuntimeError(f"signaling: SRT_PROVIDER missing srflx_*: {kv}")
    return ProviderInfo(
        service_id = kv.get("service_id", ""),
        srflx_ip   = kv["srflx_ip"],
        srflx_port = int(kv["srflx_port"]),
        lan_ip     = kv.get("lan_ip"),
        lan_port   = int(kv["lan_port"]) if kv.get("lan_port") else None,
        raw        = kv,
    )


# ── SRT rendezvous (camera-compatible options) ──────────────────────────────

def srt_rendezvous(local_port: int, peer_ip: str, peer_port: int,
                   latency_ms: int = 300) -> int:
    """Returns a connected libsrt socket handle (an int).  Raises on failure."""
    if srt.srt_startup() < 0:
        raise RuntimeError(f"srt_startup: {_srterr()}")
    s = srt.srt_create_socket()
    if s == SRT_INVALID_SOCK:
        raise RuntimeError(f"srt_create_socket: {_srterr()}")

    def opt(opt_id: int, c_val, name: str) -> None:
        ptr = ctypes.byref(c_val)
        sz  = ctypes.sizeof(c_val)
        if srt.srt_setsockopt(s, 0, opt_id, ptr, sz) == SRT_ERROR:
            srt.srt_close(s)
            raise RuntimeError(f"srt_setsockopt({name}): {_srterr()}")

    # Order matters: TRANSTYPE first, then everything else.
    opt(SRTO_TRANSTYPE,   ctypes.c_int(SRTT_FILE), "TRANSTYPE")
    opt(SRTO_MESSAGEAPI,  ctypes.c_int(0),         "MESSAGEAPI")
    opt(SRTO_RENDEZVOUS,  ctypes.c_int(1),         "RENDEZVOUS")
    opt(SRTO_LATENCY,     ctypes.c_int(latency_ms),"LATENCY")
    opt(SRTO_PEERLATENCY, ctypes.c_int(latency_ms),"PEERLATENCY")
    opt(SRTO_RCVLATENCY,  ctypes.c_int(latency_ms),"RCVLATENCY")
    opt(SRTO_RCVBUF,      ctypes.c_int(1024*1024), "RCVBUF")
    # libsrt internally multiplies CONNTIMEO by 10 for rendezvous mode, so
    # setting 2 s here gives us a 20 s wall-clock bound — long enough for a
    # healthy handshake (typical <300 ms) but short enough to fail loud.
    opt(SRTO_CONNTIMEO,   ctypes.c_int(2_000),     "CONNTIMEO")
    # Carrier-NAT keepalive: default 5 s peer-idle is shorter than typical
    # CGNAT UDP idle (Jio ~30 s).  Bump to 60 s — libsrt internally sends
    # ACK+keepalive packets, so this just controls the abort threshold.
    opt(SRTO_PEERIDLETIMEO, ctypes.c_int(60_000),  "PEERIDLETIMEO")

    def _fill_sockaddr(sa: sockaddr_in, ip: str, port: int) -> None:
        if sys.platform == "darwin":
            sa.sin_len = ctypes.sizeof(sa)
        sa.sin_family = socket.AF_INET
        sa.sin_port   = socket.htons(port)
        # sin_addr is stored in NETWORK byte order in the C struct; inet_aton
        # already returns 4 bytes in network order so we just unpack them.
        sa.sin_addr   = struct.unpack("=I", socket.inet_aton(ip))[0]

    # Bind on the same UDP port STUN discovered.
    local = sockaddr_in()
    _fill_sockaddr(local, "0.0.0.0", local_port)
    if srt.srt_bind(s, ctypes.byref(local), ctypes.sizeof(local)) == SRT_ERROR:
        msg = _srterr()
        srt.srt_close(s)
        raise RuntimeError(f"srt_bind :{local_port}: {msg}")

    # Connect (rendezvous).
    peer = sockaddr_in()
    _fill_sockaddr(peer, peer_ip, peer_port)

    print(f"[srt]   rendezvous connecting to {peer_ip}:{peer_port} ...", flush=True)
    rc = srt.srt_connect(s, ctypes.byref(peer), ctypes.sizeof(peer))
    if rc == SRT_ERROR:
        msg = _srterr()
        srt.srt_close(s)
        raise RuntimeError(f"srt_connect: {msg}")
    print(f"[srt]   connected (handle={s})", flush=True)
    return s


# ── HTTP server: jessibuca page + live.flv pump ─────────────────────────────

class FlvFanout:
    """Single-producer (SRT pump thread) → N consumers (each `/live.flv` GET).

    Keeps a frozen cache of the first PREFIX_BYTES bytes of the FLV stream
    so consumers attaching mid-flight get the FLV header + onMetaData +
    HEVC AVCDecoderConfigurationRecord (SPS/PPS) — without those ffplay's
    demuxer can't lock onto HEVC.

    Replay-from-start-of-stream is the lag source: a viewer that attaches
    T seconds into pump life experiences a T-second-of-stream-start replay
    before catching up to live.  Bounded by PREFIX_BYTES / actual-bitrate.

    Why a single frozen cache and not a sliding-window tail?  Tried that —
    naive concat of "INIT (0..N) + TAIL (M..M+K)" produces a FLV stream
    where the PrevTagSize chain breaks at the join (the byte at offset N
    isn't aligned on a tag boundary), and ffmpeg reports "Packet mismatch
    ..." and stops decoding.  Proper sliding-window requires parsing FLV
    tags to find safe-cut boundaries — deferred.

    PREFIX_BYTES tuning (HEVC sub-stream at ~7 KB/s observed bitrate):
       16 KB   — ffplay bails during probe (init segment not seen yet)
       256 KB  — probe succeeds; ffplay -framedrop catches up to live in
                 ~2-5 s after a ~30 s replay window fills, GOOD DEFAULT
       2 MB    — probe succeeds but late-attach viewers replay ~5 min of
                 stale stream-start before catching up (the original bug)
    """

    PREFIX_BYTES = 256 * 1024

    def __init__(self) -> None:
        self._lock      = threading.Lock()
        self._consumers: list[tuple[threading.Event, list[bytes]]] = []
        self._prefix    = bytearray()
        self._closed    = False

    def add_consumer(self) -> tuple[threading.Event, list[bytes]]:
        ev:  threading.Event = threading.Event()
        buf: list[bytes]      = []
        with self._lock:
            # Replay the cached prefix so the new consumer starts at the
            # FLV header (with onMetaData + AVCDecoderConfigurationRecord),
            # not mid-stream where its demuxer can't lock.
            if self._prefix:
                buf.append(bytes(self._prefix))
                ev.set()
            self._consumers.append((ev, buf))
        return ev, buf

    def remove_consumer(self, target: tuple[threading.Event, list[bytes]]) -> None:
        with self._lock:
            try:
                self._consumers.remove(target)
            except ValueError:
                pass

    def push(self, chunk: bytes) -> None:
        with self._lock:
            if len(self._prefix) < self.PREFIX_BYTES:
                need = self.PREFIX_BYTES - len(self._prefix)
                self._prefix += chunk[:need]
            for ev, buf in self._consumers:
                buf.append(chunk)
                ev.set()

    def close(self) -> None:
        with self._lock:
            self._closed = True
            for ev, _ in self._consumers:
                ev.set()

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def has_consumer(self) -> bool:
        with self._lock:
            return bool(self._consumers)


def srt_pump_thread(srt_sock: int, fanout: FlvFanout, http_req: bytes) -> None:
    """Reads from SRT forever, pushes raw bytes into the fanout.  Also patches
    the FLV header's DataOffset (camera's libflv emits 0 instead of 9)."""
    # Send the HTTP-FLV GET first.
    n = srt.srt_send(srt_sock, http_req, len(http_req))
    if n < 0:
        print(f"[srt]   srt_send(HTTP GET): {_srterr()}", flush=True)
        fanout.close()
        return
    print(f"[srt]   sent {n}B HTTP-FLV GET over SRT", flush=True)

    # Recv loop.
    bufsize = 8192
    buf = ctypes.create_string_buffer(bufsize)
    total = 0
    header_patched = False
    first_byte_seen = False
    while not fanout.closed:
        n = srt.srt_recv(srt_sock, buf, bufsize)
        if n == SRT_ERROR:
            msg = _srterr()
            if "expired" in msg.lower() or "asyncrcv" in msg.lower():
                continue
            print(f"[srt]   srt_recv failed: {msg}", flush=True)
            break
        if n > 0 and not first_byte_seen:
            first_byte_seen = True
            stamp("t_first_flv_byte")
            dump_timings()
        if n == 0:
            continue
        chunk = bytes(buf.raw[:n])
        total += n
        # FLV header DataOffset patch (bytes 5..8): camera writes 0, spec says 9.
        if not header_patched and total >= 9 and chunk[:3] == b"FLV":
            chunk = chunk[:5] + b"\x00\x00\x00\x09" + chunk[9:]
            header_patched = True
            print(f"[srt]   first FLV bytes: header patched (DataOffset 0→9)",
                  flush=True)
        elif not header_patched and total >= 9:
            header_patched = True   # don't keep checking
        fanout.push(chunk)
    print(f"[srt]   pump exit after {total}B", flush=True)
    fanout.close()


def make_http_handler(jessibuca_dir: pathlib.Path | None, fanout: FlvFanout):

    class Handler(http.server.BaseHTTPRequestHandler):
        # Quieter logs — one line per request, not three.
        def log_message(self, fmt: str, *args) -> None:
            sys.stdout.write(f"[http]  {self.address_string()} {fmt % args}\n")
            sys.stdout.flush()

        def do_GET(self) -> None:
            # Strip query string for routing only.
            path = self.path.split("?", 1)[0]
            if path == "/" or path == "/index.html":
                if jessibuca_dir is None:
                    # No browser-player assets — return a tiny landing page
                    # pointing the user at the raw FLV endpoint (ffplay/VLC).
                    body = (b"<!doctype html><meta charset=utf-8>"
                            b"<title>augentix_view</title>"
                            b"<p>SRT tunnel up.  Point ffplay/VLC at "
                            b"<code>/live.flv</code> on this host.</p>")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                # Serve a tiny shim that loads jessibuca's player.html and
                # auto-calls initPlayer(local FLV).  We can't embed in a
                # frame because jessibuca expects single-frame context, so
                # we redirect with a query string our shim parses.
                self.send_response(302)
                self.send_header("Location",
                                 "/jessibuca/player.html?stream=/live.flv")
                self.end_headers()
                return

            if path.startswith("/jessibuca/"):
                if jessibuca_dir is None:
                    self.send_error(404); return
                # Static serve from the asset bundle.
                rel = path[len("/jessibuca/"):]
                target = (jessibuca_dir / rel).resolve()
                try:
                    target.relative_to(jessibuca_dir)    # path traversal guard
                except ValueError:
                    self.send_error(403); return
                if not target.is_file():
                    self.send_error(404); return
                self.send_response(200)
                self.send_header("Content-Type", _mime(target))
                self.send_header("Content-Length", str(target.stat().st_size))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                with open(target, "rb") as f:
                    while True:
                        block = f.read(65536)
                        if not block:
                            break
                        try:
                            self.wfile.write(block)
                        except (BrokenPipeError, ConnectionResetError):
                            return
                return

            if path == "/live.flv":
                self._serve_live_flv()
                return

            if path == "/_bootstrap_beacon":
                # Diagnostic: bootstrap script calls this so we can verify
                # in the server log that the page-side JS ran.
                qs = self.path.split("?", 1)[1] if "?" in self.path else ""
                print(f"[boot]  page beacon: {qs}", flush=True)
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                return

            self.send_error(404)

        def _serve_live_flv(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "video/x-flv")
            self.send_header("Cache-Control", "no-store, no-cache, no-transform")
            self.send_header("Access-Control-Allow-Origin", "*")
            # NOTE: no Content-Length and no Connection: close.  We want HTTP/1.1
            # streaming length-of-stream until either side closes; jessibuca's
            # FLV-over-fetch reader handles that path fine.
            self.end_headers()
            consumer = fanout.add_consumer()
            ev, queue = consumer
            try:
                while not fanout.closed:
                    if not queue:
                        ev.wait(timeout=1.0)
                        ev.clear()
                        continue
                    # Drain whatever's queued in one wfile.write call.
                    chunks, queue[:] = queue[:], []
                    out = b"".join(chunks)
                    try:
                        self.wfile.write(out)
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        print("[http]  /live.flv consumer disconnected",
                              flush=True)
                        return
            finally:
                fanout.remove_consumer(consumer)
    return Handler


def _mime(p: pathlib.Path) -> str:
    ext = p.suffix.lower()
    return {
        ".html": "text/html; charset=utf-8",
        ".js":   "text/javascript; charset=utf-8",
        ".css":  "text/css; charset=utf-8",
        ".wasm": "application/wasm",
        ".png":  "image/png",
        ".jpg":  "image/jpeg",
        ".jpeg": "image/jpeg",
        ".svg":  "image/svg+xml",
        ".ico":  "image/x-icon",
    }.get(ext, "application/octet-stream")


class ThreadingHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


# ── orchestration ───────────────────────────────────────────────────────────

def main() -> int:
    stamp("t_start")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stun-host",      default="turn.devices.arcisai.io")
    ap.add_argument("--stun-port",      default=5349,  type=int)
    ap.add_argument("--signaling-host", default="signaling.devices.arcisai.io")
    ap.add_argument("--signaling-port", default=80,  type=int)
    ap.add_argument("--api-token",      default=os.environ.get("ARCISAI_API_TOKEN", ""))
    ap.add_argument("--service-id",     default="ATPL-200007-TESTA")
    ap.add_argument("--verify-token",   default=os.environ.get("ARCISAI_VERIFY_TOKEN", ""))
    ap.add_argument("--channel-path",   default="/flv/live_ch0_0.flv",
                    help="main stream (live_ch0_0) or sub (live_ch0_1)")
    ap.add_argument("--latency-ms",     default=300,   type=int,
                    help="SRT TSBPD latency buffer in ms (300ms is the stable "
                         "value that produced ~2s glass-to-glass; lower values "
                         "are worth experimenting with but only one knob at a "
                         "time, since they interact with the prefix cache size)")
    ap.add_argument("--http-port",      default=8080,  type=int)
    ap.add_argument("--jessibuca-dir",
                    default="",
                    help="Optional path to jessibuca assets (player.html + "
                         "decoder bundle).  When unset or missing, the script "
                         "still serves /live.flv for ffplay/VLC consumers but "
                         "skips the in-browser jessibuca route.")
    args = ap.parse_args()

    # jessibuca is optional now — the canonical Mac path is ffplay against
    # /live.flv.  Only enable the browser route if the asset dir is real.
    jessibuca_dir: pathlib.Path | None = None
    if args.jessibuca_dir:
        candidate = pathlib.Path(args.jessibuca_dir).resolve()
        if (candidate / "player.html").is_file():
            jessibuca_dir = candidate
        else:
            print(f"[http]  WARN: jessibuca dir lacks player.html ({candidate}); "
                  f"continuing without browser viewer.", flush=True)

    # ── 1. STUN bind ──
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    local_port = udp.getsockname()[1]
    print(f"[stun]  local UDP port={local_port}, "
          f"target={args.stun_host}:{args.stun_port}", flush=True)
    try:
        srflx_ip, srflx_port = stun_discover(
            udp, args.stun_host, args.stun_port)
    except Exception as e:
        sys.exit(f"[stun]  failed: {e}")
    stamp("t_stun_done")
    print(f"[stun]  SRFLX={srflx_ip}:{srflx_port}", flush=True)
    udp.close()
    # Brief sleep — NAT mapping survives for several seconds without traffic
    # on a typical home router, more than enough to rebind via libsrt.

    # ── 2. Signaling ──
    try:
        prov = signaling_request(
            args.signaling_host, args.signaling_port,
            service_id=args.service_id, api_token=args.api_token,
            srflx_ip=srflx_ip, srflx_port=srflx_port,
        )
    except Exception as e:
        sys.exit(f"[sig]   failed: {e}")
    stamp("t_sig_connect_ok")
    print(f"[sig]   provider SRFLX={prov.srflx_ip}:{prov.srflx_port}"
          f"  lan={prov.lan_ip}:{prov.lan_port}", flush=True)

    # ── 3. SRT rendezvous ──
    try:
        srt_sock = srt_rendezvous(local_port, prov.srflx_ip, prov.srflx_port,
                                  latency_ms=args.latency_ms)
    except Exception as e:
        sys.exit(f"[srt]   failed: {e}")
    stamp("t_srt_handshake_done")

    # ── 4. Build HTTP-FLV GET ──
    auth_b64 = "YWRtaW46YWRtaW4="   # admin:admin — present in camera config
    query    = (f"?verify={args.verify_token}"
                if args.verify_token else "")
    # NOTE: NO "Connection: close" — that tells the camera's HTTP-FLV server
    # to flush+close the SRT after the initial response, which is exactly the
    # "Invalid socket ID" + 12 KB-and-die pattern we kept seeing.  Default
    # HTTP/1.1 is keep-alive; the camera streams continuously until WE close.
    http_req = (
        f"GET {args.channel_path}{query} HTTP/1.1\r\n"
        f"Host: 127.0.0.1\r\n"
        f"Authorization: Basic {auth_b64}\r\n"
        f"User-Agent: ArcisAI-Augentix-Viewer-py/1.0\r\n"
        f"\r\n"
    ).encode("ascii")

    # ── 5. Local HTTP server + SRT pump ──
    fanout = FlvFanout()
    t = threading.Thread(
        target=srt_pump_thread, args=(srt_sock, fanout, http_req),
        daemon=True, name="srt-pump")
    t.start()

    Handler = make_http_handler(jessibuca_dir, fanout)
    server  = ThreadingHTTP(("127.0.0.1", args.http_port), Handler)
    url     = f"http://127.0.0.1:{args.http_port}/"
    if jessibuca_dir is not None:
        print(f"[http]  serving {jessibuca_dir} on {url}", flush=True)
        print(f"[http]    {url}live.flv  (raw FLV byte stream)", flush=True)
        print(f"[http]    {url}          (auto-opens jessibuca on /live.flv)",
              flush=True)
        print("        Open the second URL in Chrome to view the feed.",
              flush=True)
    else:
        print(f"[http]  serving FLV stream on {url}live.flv", flush=True)
        print(f"[http]  run:  tools/view-feed.sh   (ffplay)  or  "
              f"ffplay {url}live.flv", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[main]  shutting down...", flush=True)
    finally:
        fanout.close()
        srt.srt_close(srt_sock)
    return 0


if __name__ == "__main__":
    sys.exit(main())
