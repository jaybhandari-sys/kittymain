# kitty-augentix-camera

Production firmware platform ("kitty") for the **Augentix HC1703 / 1723 / 1753 / 1783s** IPC family.

> _Single repo of record for everything that ships on these cameras._  Sources for the SRT P2P stack, the MQTT camera client, the production launcher with watchdog + log rotation + free-space alarm, the device-bring-up tools, and the CI/CD that builds the lot.

**Current release: [`v2.0.3`](https://github.com/Adiance-Technologies/kitty-augentix-camera/releases/latest)** — single statically-merged binary (`ap2p`) replacing the legacy two-binary layout, brand-scrubbed for `strings`-resistance, ships as `Augentix.tar.gz` for the existing firmware update tool.  v2.0.3 drops the unused symlink set and `kitty.logrotate` from the bundle — see [`docs/SPACE_BUDGET.md`](docs/SPACE_BUDGET.md) for the partition math and the busybox-`cp` symlink-follow trap that triggered ENOSPC on v2.0.1.

---

## What's on the camera (v2.x)

Five files of ours on `/mny/mtd/ipc/`:

```
/mny/mtd/ipc/
├── ambicam.sh                ~16-line process supervisor (no protocol vocabulary)
└── ambicam/
    ├── ap2p                  single ARM ELF (~663 KB stripped+scrubbed)
    │                         — MQTT client + signaling client + SRT pump in one
    ├── config.json           MQTT bootstrap (broker URL + creds + serviceId)
    ├── VERSION               "2.0.1"
    ├── BUILT_AT, COMMIT      audit trail
    └── (a handful of .so symlinks — see ops/firmware-update-tool.md)
```

Runtime config (signaling host, STUN/TURN, source path, etc.) is **never persisted** — it's delivered as an **MQTT retained message** on `torque/rx/<NODE_ID>/81` and held in process memory.  Rotate it fleet-wide with one `mosquitto_pub --retain`.

## Repo layout

```
src/
  ap2p/                          v2 single-binary daemon (current)
    main.c, state.c, config.c
    mqtt_thread.c                MQTT loop + case dispatch
    legacy_cases.c               verbatim port of v1.13.x HTTP.c (81 case handlers)
    srt_thread.c                 STUN + signaling + libsrt rendezvous + HTTP-FLV pump
    edge_lite.h                  brand-neutral STUN client
    Makefile                     cross-compile + strip + scrub + patchelf
  provider-srt/                  legacy v1.13.x — kept for hot-fix paths
  mqtt-vcamclient/               legacy v1.13.x — kept for hot-fix paths
  consumer-srt-cloud/            cloud-side SRT consumer (consumer_srt.c)
  consumer-srt-android/          Android-side SRT consumer (consumer_srt_p2p.c)
branding/
  manifest.toml                  brand contract (prefix, forbidden tokens, allowed NEEDED)
  scrub.py                       post-link string-literal zero-fill
  verify.sh                      CI brand-leak gate
third_party/
  libsrt/                        vendored Haivision libsrt (statically linked)
  libjuice/                      vendored libjuice (ICE/STUN/TURN — statically linked)
toolchain/                       arm-augentix-linux-uclibcgnueabihf docs + fetch script
build/                           Makefile + redeploy scripts (cross-compile orchestration)
deploy/
  ambicam.sh                     v1.13.x launcher (full watchdog) — for v1.x compat
  ambicam.sh.v2                  v2 minimal supervisor (~16 lines, brand-clean)
  logrotate.d/kitty
  systemd/                       systemd unit files for the cloud-side services
ops/
  firmware-update-tool.md        ★ Augentix.tar.gz contract for the deploy tool
  camera-deploy-guide.md         step-by-step camera install (telnet path)
  dev-integration-guide.md       Android consumer integration handoff
  broker-provisioning-runbook.md MQTT retained-message workflow
  cutover-v2.0.sh                one-shot per-camera v1→v2 config translator
  v2.0-cutover-runbook.md        v1.13.x → v2.0 cutover workflow
  gcp-mtls-migration.md          v2.1 plan: TLS+mTLS on :443
  realtime-latency.md            latency-tuning notes (open work)
  mqtt-provisioning.md           MQTT topic conventions
tools/
  augentix_view.py               Mac dev viewer (STUN + signaling + SRT rendezvous + HTTP-FLV proxy)
  view-feed.sh                   Mac wrapper to launch ffplay against augentix_view.py
  diagnostics/                   python helpers (e2e tests, log diag)
.github/workflows/
  build-augentix.yml             cross-compile + brand-leak gate + dual release tarballs
```

## Build

CI does everything on every tag — fresh checkout, fetch the toolchain from GCS, cross-compile libsrt, cross-compile `ap2p`, strip + brand-scrub, hard-fail on any brand leak (`v2.x` tags only), publish both release tarballs.

Local build (only useful if you have the Augentix toolchain on a Linux host):

```sh
cd src/ap2p
make all REPO_ROOT=$PWD/../..  \
         TOOLCHAIN_DIR=/opt/augentix-tc/arm-augentix-linux-uclibcgnueabihf
```

## Releases

Every tag produces **two** assets on GitHub Releases:

| Asset | Format | Consumer |
|---|---|---|
| `Augentix.tar.gz` | extracts to `AugenTix/` | the existing firmware-update tool — see [`ops/firmware-update-tool.md`](ops/firmware-update-tool.md).  Drop-in for first-flash deployments. |
| `kitty-augentix-camera-X.Y.Z.tar.gz` | extracts to `ambicam/` | operator deploys (`ops/camera-deploy-guide.md`) and ad-hoc telnet pushes |

For fleet rollout: upload `Augentix.tar.gz` to `prong.arcisai.io/protected/Augentix/A_Series_X.Y.Z/Augentix.tar.gz` to match the firmware tool's expected URL.

## Deploy to a single camera

**Option A — firmware update tool** (standard path; see `ops/firmware-update-tool.md`).

**Option B — manual telnet push** (for dev cameras): see `ops/camera-deploy-guide.md`.

**Option C — v1.13.x → v2.0 cutover** of a specific camera: see `ops/v2.0-cutover-runbook.md`.

## View a feed from your Mac

```sh
tools/view-feed.sh
```

One command. Defaults baked in for the GCP staging stack — connects to `signaling.devices.arcisai.io:80`, runs full STUN → signaling → SRT rendezvous → HTTP-FLV pump → ffplay.  ~2 s glass-to-glass.

For a custom service-id / channel:
```sh
SERVICE_ID=ATPL-XXXXXX-YYYY \
CHANNEL_PATH=/flv/live_ch0_0.flv \
  tools/view-feed.sh
```

## Open items

- [ ] **v2.1: TLS+mTLS migration** — see `ops/gcp-mtls-migration.md`.  `KITTY_TLS_FLAG=-DKITTY_TLS` build, `signaling.devices.arcisai.io:443` with nginx mTLS termination.
- [ ] **v2.1: Phase F branded `.so` SONAMEs** — patchelf the actual `.so` files (libpaho/libcurl) to carry `libap2p_*` SONAMEs, so `readelf -d ap2p` becomes brand-clean too (currently it still shows the upstream names — `strings ap2p` is already clean).
- [ ] **v3.0: MQTT broker URL failover / DR** — `mqttUrl_BKP` is in config.json but not yet honored by ap2p.
- [ ] **Latency**: camera GOP is 2 s → push it to 0.5 s via MQTT-config; reach <500 ms glass-to-glass.  See `ops/realtime-latency.md`.
- [ ] **provider_srt `LATENCY_MS` wiring**: parsed by the daemon but never applied to the SRT socket.  One-line fix in `srt_thread.c:set_srt_options_caller()`.
- [ ] **Rotate the leaked staging secrets** (current `p2p-server-api-token-change-me` / `Raptor@0` / `turnpassword123` / verify token) — see `SECURITY.md`.

## Provenance

- `src/ap2p/srt_thread.c` — ported from `src/provider-srt/provider_srt.c` (originally from [`Adiance-Technologies/Signalling-Server`](https://github.com/Adiance-Technologies/Signalling-Server) `main` @ commit `f70f518`).
- `src/ap2p/legacy_cases.c` — near-verbatim copy of `src/mqtt-vcamclient/HTTP.c` (originally from [`hardiksa/Eco-Series-Kitty`](https://github.com/hardiksa/Eco-Series-Kitty) `main`).  Three surgical changes: `main()` wrapped out, `messageArrived` renamed, case-81 short-circuited.
- `src/ap2p/edge_lite.h` — brand-neutral rename of `src/provider-srt/stun_lite.h`.
- `third_party/libjuice/`, `third_party/libsrt/` — vendored MPL-2.0 sources.
- `deploy/ambicam.sh`, `deploy/ambicam.sh.v2` — written for this repo.
- `tools/augentix_view.py`, `tools/view-feed.sh` — imported from `hardiksa/nvr-cloud-platform` `tools/`.

## License

Internal — proprietary to Adiance Technologies / ArcisAI.  See individual `LICENSE` files inside `third_party/` for vendored components (libjuice is MPL-2.0, libsrt is MPL-2.0; paho-mqtt, libcurl, OpenSSL each retain their upstream licenses — shipped on-camera as separate `.so` files).
