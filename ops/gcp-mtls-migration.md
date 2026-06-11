# GCP migration — everything on `:443` behind nginx mTLS

**Status**: PLAN.  No production changes yet.  Reviewed-by: \_\_\_.  Cut-over scheduled: \_\_\_.

---

## Goal

Move every camera-facing endpoint behind the existing GCP nginx wildcard-TLS
fleet on **port 443 only**, with **mTLS client-cert verification** for any
endpoint that carries device identity or signaling.

End state, all on `34.100.143.36 / *.devices.arcisai.io`:

| Subdomain                          | Listener                          | Backend                    | mTLS?       |
|------------------------------------|-----------------------------------|----------------------------|-------------|
| `turn.devices.arcisai.io`          | TCP/443 (TLS-TURN), UDP/443 (STUN binding) | coTURN                  | n/a (TURN-auth) |
| `signaling.devices.arcisai.io`     | TCP/443 (HTTPS)                   | signaling_server (WebSocket) | **yes**   |
| `consumer-api.devices.arcisai.io`  | TCP/443 (HTTPS)                   | consumer_api               | **yes**     |
| `*.devices.arcisai.io` catchall    | TCP/443 (HTTPS)                   | reject (403)               | yes         |

DigitalOcean infra (`142.93.223.221:8888` signaling, `157.245.101.250` MQTT,
`143.244.139.222` p2p consumer) sunsetted **after** all cameras flipped over.

---

## Why this is hard (the careful part)

The existing GCP nginx is a production HTTPS server with many tenants on
`:443` (see `/etc/nginx/conf.d/arcisai-mtls.conf`).  It uses **`http {}`
context only** — no `stream {}` block, no SNI-passthrough TLS.

Our signaling protocol is **raw TCP with 4-byte-length-prefixed INI frames**
(see `signaling_server.cpp` and `sig_send` in `provider_srt.c`).  This does
**not** fit naturally into the existing HTTPS nginx pattern.

We have three structurally different ways to put it on `:443` behind mTLS:

| Approach | Code change | nginx change | Risk | Reversibility |
|----------|-------------|--------------|------|---------------|
| **A. Wrap signaling protocol in WebSocket-over-HTTPS** | `signaling_server`, `provider_srt`, `consumer_srt`, `consumer_srt_p2p` (Android) | Add one `server { server_name signaling.devices… }` block — same as existing services | Low — only the new endpoint can break; existing tenants untouched | High — keep DO listener up for fallback during cut-over |
| **B. `stream { ssl_preread }` SNI router on `:443`** to dispatch raw-TCP signaling SNI to internal `127.0.0.1:8888`, and all other SNI to internal `127.0.0.1:8443` (where the existing `http {}` block moves) | None | **Major** — every existing server block changes its `listen` address | High — config error breaks every tenant | Low — rollback requires touching every existing block |
| **C. Second external IP for the camera plane** — a separate nginx instance owns `:443` on the new IP with a stream block, signaling lives there | None | New IP, new conf file, no overlap | Low — isolated | Trivial — drop the new IP |

**Recommendation**: **A**.  It aligns with the existing pattern, uses the
same wildcard cert and CA, gets free per-DN audit logging + rate limiting,
and the migration is reversible per-camera by config.  Cost is code work,
not infra risk.  The code work is bounded (we own all four binaries).

---

## Phase 1 — proof on staging (no production touch)

1. Stand up a **staging signaling instance** on `arcisai-analysis-server`
   (10.160.0.2 — already isolated, separate firewall rules).  Pick a new
   subdomain like `staging-signaling.devices.arcisai.io` and use the same
   wildcard cert.
2. Implement WebSocket transport in `signaling_server.cpp`:
   ```
   POST /srt-signaling/register          → returns service_id record
   POST /srt-signaling/request           → returns SRT_PROVIDER
   GET  /srt-signaling/events  (long-poll or SSE) → SRT_PEER pushes
   ```
   Or, alternative: single WebSocket endpoint `wss://…/srt-signaling`
   that carries the existing length-prefixed INI frames inside WS frames.
   **Recommend the WebSocket variant** — minimal protocol change, the
   length-prefixed wire format is reused intact.
3. Update `provider_srt.c` to use **libcurl + WebSocket** (curl ≥7.86) or
   a tiny embedded WS client (`tweetnacl`+`mbedtls`).  uclibc on the camera
   already has curl available (`libcurl.so.4` is in the AmbiCam dir).
4. Update `consumer_srt.c` (cloud) and `consumer_srt_p2p.c` (Android)
   matching changes.
5. Wire mTLS: clients present their device cert (already provisioned for
   MQTT).  nginx verifies `$ssl_client_verify=SUCCESS`, rejects otherwise.
6. End-to-end test on one staging camera (NOT the production fleet).

## Phase 2 — production cut-over, per camera

Pre-cut-over checklist (run on each camera before flipping):
```
[ ] device cert valid + not within 30 days of expiry
[ ] /mny/mtd/ipc free space > 200 KB headroom
[ ] kitty-augentix-camera version ≥ 1.0.0 (with WebSocket support)
[ ] MQTT control plane healthy (alarm-publish round trip works)
```

Cut-over (per camera, MQTT-pushed config change, atomic):
```ini
# /mny/mtd/ipc/ambicam/provider_srt.conf
SIGNALING_HOST=signaling.devices.arcisai.io
SIGNALING_PORT=443
SIGNALING_SCHEME=wss            # NEW key — version-gated by the new binary
SIGNALING_PATH=/srt-signaling
SIGNALING_CLIENT_CERT=/etc/jffs2/device.crt
SIGNALING_CLIENT_KEY=/etc/jffs2/device.key
SIGNALING_CA=/etc/jffs2/ca.crt
```
- Old `provider_srt` binary ignores the new keys (rolling back is just an
  MQTT push reverting the host).
- New `provider_srt` binary that ships in `kitty-augentix-camera ≥ 1.0.0`
  uses the new path.

Roll-back: MQTT-push the old `SIGNALING_HOST=142.93.223.221` value.
Old binary still works (we keep DO listener up for 30 days post-cut-over).

## Phase 3 — sunset DO infrastructure

After 100 % of fleet flipped + 14 days observation:
- Stop `142.93.223.221:8888` listener.
- Update DNS to remove DO-side records (`*.p2p.arcisai.io` wildcard).
- Document in the runbook.

---

## What changes on the GCP VM (the nginx side)

A single new server block, added to `arcisai-mtls.conf`:

```nginx
# ---------------- SRT signaling (WebSocket-over-HTTPS, mTLS) ---------------
server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name signaling.devices.arcisai.io;

    access_log /var/log/nginx/srt-signaling-access.log arcisai_mtls_audit;
    error_log  /var/log/nginx/srt-signaling-error.log warn;

    include /etc/nginx/conf.d/ssl-params.conf;

    ssl_verify_client on;
    if ($ssl_client_verify != SUCCESS) { return 403; }

    # Burst slightly higher than other tenants — signaling is bursty during
    # rendezvous (one camera can fire 4-6 messages in <100ms).
    limit_req zone=p2p_limit burst=400 nodelay;

    location /srt-signaling {
        # WebSocket upgrade — keep the upstream connection alive forever
        # (signaling is meant to be persistent; PING/PONG keepalive is in
        # the application protocol).
        proxy_pass http://127.0.0.1:8888;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header X-Client-DN $ssl_client_s_dn;
        proxy_set_header X-Client-Serial $ssl_client_serial;
        proxy_set_header X-Client-Cert-Verified $ssl_client_verify;
        proxy_connect_timeout 10s;
        proxy_send_timeout    7d;
        proxy_read_timeout    7d;
        proxy_buffering       off;
    }

    location /.health {
        access_log off;
        return 200 "OK\n";
        add_header Content-Type text/plain;
    }
}
```

**No changes to any existing server block** — that's the safety guarantee.

### coTURN side

Add a single line to `/etc/turnserver.conf`:
```conf
# UDP:443 for STUN binding requests on locked-down corporate networks.
listening-port=443
```
(coTURN already serves TLS-TURN on `5349/tcp`; cameras can keep using that
URL or be flipped via the same MQTT config push.)

GCP firewall rule update (`gcloud compute firewall-rules`):
```
arcisai-platform-server allow ingress tcp:443  (already open)
arcisai-platform-server allow ingress udp:443  (NEW — add)
```

---

## Verification gates before announcing cut-over complete

| Check | How |
|-------|-----|
| nginx config valid       | `sudo nginx -t` on the VM, then `systemctl reload nginx` |
| Existing tenants healthy | `curl -sk https://ems.devices.arcisai.io/.health` etc. for every active subdomain |
| Signaling reachable      | `wscat --cert dev.crt --key dev.key --ca ca.crt wss://signaling.devices.arcisai.io:443/srt-signaling` |
| Camera flips cleanly     | MQTT-push to one camera; verify SRT pipeline up; verify rollback by reverting config |
| Audit log format         | Tail `/var/log/nginx/srt-signaling-access.log` — confirm DN + serial captured |

---

## Why not just put signaling on a different port?

Two reasons:
1. Carrier / corporate firewalls.  India's enterprise NATs (CGNAT included)
   block outbound non-HTTPS-ports for many subscribers.  Cameras shipping
   into customer LANs hit this constantly.  `:443` works everywhere.
2. Operational uniformity.  One nginx config, one cert, one audit log
   format, one rate-limit policy.  Diagnostics scale.

---

## Out of scope for this migration

- Latency optimization (separate work — see `ops/realtime-latency.md`).
- Single-frame GOP for true sub-second TTFB (camera firmware change).
- DTLS-SRTP through TURN (not currently enabled).
